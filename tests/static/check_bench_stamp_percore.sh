#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The microbench switch bracket's stamp cell, read out of the LINKED IMAGE: above one kernel
# core it must be addressed off this core's per-CPU base and never off a link-time address.
#
# REFUSED: a switch body that forms any link-time address at all, one that never reads the
# per-CPU base, and a body this reader cannot decode.
#
# WHY THIS IS STRUCTURAL AND NOT A RUNTIME BOUND. A stamp shared between cores subtracts one
# core's open from another's close, which wraps near 2^32 when the peer opened later. That
# interleaving never happens: the kernel lock spans the whole bracket (kickos_switch_unlock
# runs from inside it, below the close), so one core at a time is ever between an open and a
# close and a shared cell reads back clean. A runtime arm on the sample values would therefore
# pass on the defect.
#
# So the claim is the ADDRESSING and not the readings, and it holds whatever the lock does.
#
# The per-arch triple below is the whole model. AN ARCH THIS FILE DOES NOT CARRY IS A REFUSAL.
#
# usage: check_bench_stamp_percore.sh <elf> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_stamp_percore.sh <elf> <objdump> <arch>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
arch="${3:?$_usage}"

# SYM: the one body that carries the bracket. BASE: the instruction that names this core's
# block. PCREL: the instruction that forms a link-time address, which is what a shared cell
# needs and a per-core cell does not.
case "$arch" in
    armv8a)
        SYM=kickos_armv8a_switch_now
        BASE='tpidr_el1'
        PCREL='adrp'
        ;;
    rv64imac)
        SYM=kickos_rv64_switch_now
        BASE='sscratch'
        PCREL='auipc'
        ;;
    *)
        fail "arch '$arch' is built above one kernel core with KICKOS_BENCH and this gate
  carries no model of its switch stamp, so nothing here can say whether two cores share one
  word. Add the arch's switch symbol, its per-CPU base instruction and its link-time address
  instruction to the table in this file" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Emits exactly one record: the body's instruction count, its per-CPU base reads and its
# link-time address formations.
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk ahead of this file.
cat > "$TMP/reader.awk" <<'AWK'
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    sub(/[ \t]*\/\/.*$/, "", text)
    sub(/[ \t]*#.*$/, "", text)
    n++
    if (text ~ base_re) { base++ }
    if (text ~ pcrel_re) { pcrel++ }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    printf "COUNTS %d %d %d\n", n, base + 0, pcrel + 0
}
AWK

read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v base_re="$BASE" -v pcrel_re="$PCREL"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn.
{
    echo '0000000000001000 <planted_switch>:'
    echo "    1000:	nop"
    case "$arch" in
        armv8a)
            echo "    1004:	mrs	x3, tpidr_el1"
            echo "    1008:	add	x3, x3, #0x20"
            echo "    100c:	str	w2, [x3]" ;;
        rv64imac)
            echo "    1004:	csrr	t1,sscratch"
            echo "    1008:	addi	t1,t1,24"
            echo "    100c:	sw	t0,0(t1)" ;;
    esac
} > "$TMP/ctl_percore"

{
    echo '0000000000001000 <planted_switch>:'
    echo "    1000:	nop"
    case "$arch" in
        armv8a)
            echo "    1004:	adrp	x3, 2000 <g_bench_sw_start>"
            echo "    1008:	add	x3, x3, #0x10"
            echo "    100c:	str	w2, [x3]" ;;
        rv64imac)
            echo "    1004:	auipc	t1,0x1"
            echo "    1008:	addi	t1,t1,24"
            echo "    100c:	sw	t0,0(t1)" ;;
    esac
} > "$TMP/ctl_shared"

{
    echo '0000000000001000 <planted_switch>:'
    echo "    1000:	nop"
    echo "    1004:	nop"
} > "$TMP/ctl_nostamp"

ctl="$(read_body "$TMP/ctl_percore" planted_switch)"
case "$ctl" in
    "COUNTS 4 1 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that addresses its stamp off the
  per-CPU base, so it cannot recognise the shape this gate requires and every verdict below is
  meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_shared" planted_switch)"
case "$ctl" in
    "COUNTS 4 0 1") ;;
    *) fail "the reader answered [$ctl] for a planted body that forms a link-time address for
  its stamp. That is the defect this gate exists to catch, so a reader that does not report it
  cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_nostamp" planted_switch)"
case "$ctl" in
    "COUNTS 2 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying neither form, so an image
  whose bracket was deleted would not be reported as such" ;;
esac

ctl_dead_reader "$(read_body "$TMP/ctl_percore" a_symbol_no_listing_carries)" \
    "a renamed switch body would read as a clean one"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the switch bracket's stamp cell in $elf =="

rec="$(read_body "$TMP/dis" "$SYM")"
kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
total="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
base="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
pcrel="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
case "$kind" in
    NOSYM)
        fail "the disassembly of $elf carries no body for '$SYM', so the reader started
  nowhere. The switch body was renamed, or the disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    COUNTS) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

require_number "$total" "the instruction count of $SYM"
require_number "$base" "the per-CPU base read count in $SYM"
require_number "$pcrel" "the link-time address count in $SYM"
echo "   corpus: $total instruction(s) in $SYM, $base per-CPU base read(s), $pcrel link-time
  address(es)"

rc=0

if [ "$pcrel" -ne 0 ]; then
    bad "the body of '$SYM' in $elf forms $pcrel link-time address(es). Every datum this body
  touches above one kernel core is per core, so a link-time address here is a cell two cores
  write: one core's close then subtracts another core's open, which wraps near 2^32 whenever
  the peer opened later and reads as a plausible switch cost otherwise"
fi

# The open and the close each reach it, so one read is half a bracket.
if [ "$base" -lt 2 ]; then
    bad "the body of '$SYM' in $elf reads the per-CPU base $base time(s), where the bracket's
  open and its close each owe one. Either the stamp is not per core, or the bracket is no
  longer in this body and this gate is reading something that cannot carry the defect"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: '$SYM' stamps its switch window through $base per-CPU base read(s) and forms no
  link-time address"
exit 0
