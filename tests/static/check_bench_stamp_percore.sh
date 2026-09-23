#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The microbench switch bracket's stamp cell, read out of the linked image: above one kernel
# core it must be reached per core, so one core's close can never subtract another core's open.
#
# The claim is the addressing and not the readings, and it holds whatever the lock does. A
# stamp shared between cores subtracts one core's open from another's close, which wraps near
# 2^32 when the peer opened later. That interleaving never happens: the kernel lock spans the
# whole bracket (kickos_switch_unlock runs from inside it, below the close), so one core at a
# time is ever between an open and a close and a shared cell reads back clean. A runtime arm on
# the sample values would therefore pass on the defect.
#
# How a core is reached is the arch's business and not the claim. Two realisations are modelled
# here, and mistaking either for the claim refuses a correct image:
#
#   base   the address comes off a per-CPU base register, and a link-time address in the body
#          is therefore a cell two cores write. armv8a and rv64imac.
#   index  there is no per-CPU base register on the part at all, so the cell is a link-time
#          array base indexed by this core's identity. A link-time address is then not merely
#          tolerable, it is mandatory: Xtensa has no large immediate and forms every global
#          address through the literal pool, so "no link-time address" is the wrong assertion
#          and would refuse every correct lx6 image. What carries the weight instead is that
#          every stamp base is combined with an identity-derived value before it is
#          dereferenced. lx6.
#
# Refused: under `base`, a switch body that forms any link-time address at all and one that
# reads the per-CPU base only once. Under `index`, a stamp base dereferenced with no identity
# read and no scaled add between, which is exactly the shared cell. Under both, a body this
# reader cannot decode.
#
# A body that addresses neither way is unknown and not a finding: the opening landmark is the
# shape the rule is stated over, so a body carrying none is a forwarder or a bracket that left,
# and tests/lib/objdump_window.awk refuses it by name rather than letting a count of zero read
# as "the stamp is not per core".
#
# The per-arch table below is the whole model. An arch not in it is refused rather than skipped.
#
# usage: check_bench_stamp_percore.sh <elf> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_stamp_percore.sh <elf> <objdump> <arch>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
arch="${3:?$_usage}"

# SYMS: the bodies that carry the bracket, and under `index` the two halves are not in one.
# BRANCH: the mnemonics whose operand names an instruction boundary, for gate.sh's synced_body.
# Under `base`: BASE names this core's block, PCREL forms a link-time address.
# Under `index`: STAMP names a stamp cell in the literal pool, IDENT reads this core's
# identity, and IDENT_CALL is the callee that returns it where the compiler called out.
case "$arch" in
    armv8a)
        MODE=base
        SYMS=kickos_armv8a_switch_now
        BRANCH='^(b|bl|b[.][a-z]+|cbn?z|tbn?z)$'
        BASE='tpidr_el1'
        PCREL='adrp'
        ;;
    rv64imac)
        MODE=base
        SYMS=kickos_rv64_switch_now
        BRANCH='^(j|jal|b(eq|ne|lt|ge|ltu|geu|eqz|nez|lez|gez|ltz|gtz|gt|le|gtu|leu))$'
        BASE='sscratch'
        PCREL='auipc'
        ;;
    lx6)
        MODE=index
        # The entry banks the previous switch's cost and opens the next; the close stands past
        # the retw, in arch_switch. tests/static/check_bench_xtensa_stamp.sh states why.
        SYMS='xtensa_switch arch_switch'
        BRANCH='^(b[a-z]*([.]n)?|j|loop[a-z]*)$'
        STAMP='<g_bench_sw_(start|end)>'
        IDENT='^rsr[.]prid$'
        IDENT_CALL='<arch_cpu_id>'
        ;;
    *)
        fail "arch '$arch' is built above one kernel core with KICKOS_BENCH and this gate
  carries no model of its switch stamp, so nothing here can say whether two cores share one
  word. Add the arch's switch bodies and the shape its per-core addressing takes to the table
  in this file, under 'base' where the part has a per-CPU base register and under 'index' where
  it reaches a per-core cell by indexing a link-time array" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the readers --------------------------------------------------------------
# `seen`, the body scope and every refusal above come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead of these.
#
# `base` emits the body's instruction count, its per-CPU base reads and its link-time address
# formations.
cat > "$TMP/reader_base.awk" <<'AWK'
{
    if (win_text ~ base_re) { base++ }
    if (win_text ~ pcrel_re) { pcrel++ }
}
END { printf "COUNTS %d %d %d\n", win_n, base + 0, pcrel + 0 }
AWK

# `index` emits the body's instruction count, the stamp accesses reached through an indexed
# base, and the stamp accesses reached off a bare link-time base, which is the shared cell.
#
# Three register marks, and only `ident` propagates freely. Widening what counts as an identity
# can only ACCEPT an index this reader would otherwise call blind; it can never turn a bare
# base into an indexed one, so the half that refuses the defect stays exact.
#
# A window rotation and a call both rename every register tracked here, so both drop the whole
# state rather than carrying a name across. A call to the identity callee then re-marks the
# register the windowed ABI returns in, which is a2 rotated by the call's own width.
cat > "$TMP/reader_index.awk" <<'AWK'
function forget(    r)
{
    for (r in cell) { delete cell[r] }
    for (r in ident) { delete ident[r] }
    for (r in pc) { delete pc[r] }
}
function drop(r)
{
    delete cell[r]
    delete ident[r]
    delete pc[r]
}
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    sub(/[ \t]*$/, "", text)
    mn = text
    sub(/[ \t].*$/, "", mn)
    ops = ""
    if (text ~ /[ \t]/)
    {
        ops = text
        sub(/^[^ \t]+[ \t]+/, "", ops)
    }
    nops = split(ops, o, /,[ \t]*/)
    for (i = 1; i <= nops; i++) { gsub(/^[ \t]+|[ \t]+$/, "", o[i]) }

    # The literal pool is resolved by the disassembler, which prints the symbol it points at,
    # so a stamp cell is named in the instruction stream and needs no symbol table of its own.
    if (mn == "l32r" && nops == 2 && o[2] ~ cell_re)
    {
        drop(o[1])
        cell[o[1]] = 1
        next
    }
    if (mn ~ id_re && nops >= 1)
    {
        drop(o[1])
        ident[o[1]] = 1
        next
    }
    if (mn ~ /^callx?[0-9]+$/)
    {
        ret = ""
        if (index(text, id_call) > 0)
        {
            if (mn ~ /4$/) { ret = "a6" }
            if (mn ~ /8$/) { ret = "a10" }
            if (mn ~ /12$/) { ret = "a14" }
        }
        forget()
        if (ret != "") { ident[ret] = 1 }
        next
    }
    if (mn == "rotw")
    {
        forget()
        next
    }
    if (mn ~ /^add(x[248])?([.]n)?$/ && nops == 3)
    {
        indexed = 0
        anybase = 0
        anyid = 0
        if ((o[2] in cell) && (o[3] in ident)) { indexed = 1 }
        if ((o[3] in cell) && (o[2] in ident)) { indexed = 1 }
        if ((o[2] in cell) || (o[3] in cell)) { anybase = 1 }
        if ((o[2] in ident) || (o[3] in ident)) { anyid = 1 }
        drop(o[1])
        if (indexed) { pc[o[1]] = 1 }
        else if (anybase) { cell[o[1]] = 1 }
        else if (anyid) { ident[o[1]] = 1 }
        next
    }
    if (mn ~ /^(l(8u|16u|32)i|s(8|16|32)i)([.]n)?$/ && nops == 3)
    {
        if (o[2] in pc) { percore++ }
        else if (o[2] in cell) { shared++ }
        if (mn ~ /^l/) { drop(o[1]) }
        next
    }
    if (nops >= 1)
    {
        anyid = 0
        for (i = 2; i <= nops; i++)
        {
            if (o[i] in ident) { anyid = 1 }
        }
        drop(o[1])
        if (anyid) { ident[o[1]] = 1 }
    }
}
END { printf "IDX %d %d %d\n", win_n, percore + 0, shared + 0 }
AWK

read_body() { # <listing> <symbol>
    if [ "$MODE" = base ]; then
        scoped_body "$TMP/reader_base.awk" "$1" "$2" \
            -v base_re="$BASE" -v pcrel_re="$PCREL" -v win_open="$BASE|$PCREL"
        return
    fi
    scoped_body "$TMP/reader_index.awk" "$1" "$2" \
        -v cell_re="$STAMP" -v id_re="$IDENT" -v id_call="$IDENT_CALL" -v win_open="$STAMP"
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
        lx6)
            echo "    1004:	l32r	a6, 1100 <pool+0x0> (3ffb5748 <g_bench_sw_end>)"
            echo "    1007:	rsr.prid	a5"
            echo "    100a:	extui	a5, a5, 1, 1"
            echo "    100d:	addx4	a6, a5, a6"
            echo "    1010:	l32i.n	a4, a6, 0"
            echo "    1012:	s32i.n	a7, a6, 0" ;;
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
        lx6)
            echo "    1004:	l32r	a6, 1100 <pool+0x0> (3ffb5748 <g_bench_sw_end>)"
            echo "    1007:	l32i.n	a4, a6, 0"
            echo "    100a:	movi.n	a7, 0"
            echo "    100c:	s32i.n	a7, a6, 0" ;;
    esac
} > "$TMP/ctl_shared"

# The forwarder: a body that addresses nothing at all, which is the record a body carrying the
# bracket and sharing its cell would otherwise be indistinguishable from.
{
    echo '0000000000001000 <planted_switch>:'
    case "$arch" in
        armv8a)   echo "    1000:	b	2000 <kickos_armv8a_switch_now_real>" ;;
        rv64imac) echo "    1000:	j	2000 <kickos_rv64_switch_now_real>" ;;
        lx6)      echo "    1000:	j	2000 <xtensa_switch_real>" ;;
    esac
    echo "    1004:	nop"
} > "$TMP/ctl_nostamp"

if [ "$MODE" = base ]; then
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
        *) fail "the reader answered [$ctl] for a planted body that forms a link-time address
  for its stamp. That is the defect this gate exists to catch, so a reader that does not report
  it cannot go red" ;;
    esac
else
    ctl="$(read_body "$TMP/ctl_percore" planted_switch)"
    case "$ctl" in
        "IDX 7 2 0") ;;
        *) fail "the reader answered [$ctl] for a planted body that indexes its stamp base by
  an identity read before touching it, so it cannot recognise the shape this gate requires and
  every verdict below is meaningless" ;;
    esac

    ctl="$(read_body "$TMP/ctl_shared" planted_switch)"
    case "$ctl" in
        "IDX 5 0 2") ;;
        *) fail "the reader answered [$ctl] for a planted body that loads a stamp base and
  dereferences it with no identity read and no scaled add between. That is the shared cell this
  gate exists to catch, so a reader that does not report it cannot go red" ;;
    esac

    # Indexed, but by a value this body loaded out of memory rather than by the core's
    # identity. Every instruction of the per-core shape is present except the one that makes
    # the index mean a core, and a reader that counted the scaled add alone would pass it.
    {
        echo '0000000000001000 <planted_switch>:'
        echo "    1000:	l32i.n	a5, a2, 8"
        echo "    1003:	l32r	a6, 1100 <pool+0x0> (3ffb5748 <g_bench_sw_end>)"
        echo "    1006:	addx4	a6, a5, a6"
        echo "    1009:	s32i.n	a7, a6, 0"
    } > "$TMP/ctl_blind"
    ctl="$(read_body "$TMP/ctl_blind" planted_switch)"
    case "$ctl" in
        "IDX 4 0 1") ;;
        *) fail "the reader answered [$ctl] for a planted body whose stamp base is scaled and
  added to a word loaded out of a structure. Nothing there names a core, so a reader that reads
  the scaled add as the per-core shape passes a cell every core lands on the same slot of" ;;
    esac

    # The identity out of a CALL rather than a register read, which is the shape the close
    # half takes: the windowed ABI returns it in a2 rotated by the call's width.
    {
        echo '0000000000001000 <planted_switch>:'
        echo "    1000:	call8	2000 <arch_cpu_id>"
        echo "    1003:	l32r	a8, 1100 <pool+0x0> (3ffb5748 <g_bench_sw_end>)"
        echo "    1006:	addx4	a10, a10, a8"
        echo "    1009:	s32i.n	a3, a10, 0"
    } > "$TMP/ctl_callid"
    ctl="$(read_body "$TMP/ctl_callid" planted_switch)"
    case "$ctl" in
        "IDX 4 1 0") ;;
        *) fail "the reader answered [$ctl] for a planted body that takes its core index from a
  call to the identity helper. That is how the half past the retw reaches its slot, so a reader
  that only knows the inline register read refuses a correct close" ;;
    esac

    # The same body with the call to some OTHER callee: the return register is then whatever
    # that callee left, and indexing by it is the blind index again.
    {
        echo '0000000000001000 <planted_switch>:'
        echo "    1000:	call8	2000 <sched_pick>"
        echo "    1003:	l32r	a8, 1100 <pool+0x0> (3ffb5748 <g_bench_sw_end>)"
        echo "    1006:	addx4	a10, a10, a8"
        echo "    1009:	s32i.n	a3, a10, 0"
    } > "$TMP/ctl_othercall"
    ctl="$(read_body "$TMP/ctl_othercall" planted_switch)"
    case "$ctl" in
        "IDX 4 0 1") ;;
        *) fail "the reader answered [$ctl] for a planted body that indexes its stamp base by
  the return value of a call that is not the identity helper, so any call at all would stand in
  for the one that names a core" ;;
    esac
fi

ctl="$(read_body "$TMP/ctl_nostamp" planted_switch)"
case "$ctl" in
    "NOOPEN 2") ;;
    *) fail "the reader answered [$ctl] for a planted two-instruction forwarder, which
  addresses its stamp neither way. Read as a record it reports zero per-core accesses, which is
  what a shared cell also reports, so a bracket that moved behind a forwarder goes red as a
  stamp that is not per core" ;;
esac

ctl_dead_reader "$(read_body "$TMP/ctl_percore" a_symbol_no_listing_carries)" \
    "a renamed switch body would read as a clean one"

synced_body_control

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the switch bracket's stamp cell in $elf =="

rc=0

for sym in $SYMS; do
    synced_body "$TMP/body" "$TMP/dis" "$sym" "$elf" "$objdump" "$BRANCH"
    rec="$(read_body "$TMP/body" "$sym")"
    kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
    total="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
    case "$kind" in
        NOSYM)
            fail "the disassembly of $elf carries no body for '$sym', so the reader started
  nowhere. The switch body was renamed, or the disassembler's output shape has moved" ;;
        NOINSN)
            fail "the body of '$sym' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
        NOOPEN)
            fail "the body of '$sym' in $elf addresses no stamp cell at all across its $total
  instruction(s). The bracket is not in this body, so what this gate has is UNKNOWN and not a
  stamp two cores share. The symbol names a forwarder, or the stamping moved to a callee" ;;
        COUNTS|IDX) ;;
        *)
            fail "the reader emitted [$rec] for '$sym', a record this gate does not model" ;;
    esac
    require_number "$total" "the instruction count of $sym"

    if [ "$MODE" = base ]; then
        base="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
        pcrel="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
        require_number "$base" "the per-CPU base read count in $sym"
        require_number "$pcrel" "the link-time address count in $sym"
        echo "   $sym: $total instruction(s), $base per-CPU base read(s), $pcrel link-time
  address(es), $SYNCED_N re-decode(s)"

        if [ "$pcrel" -ne 0 ]; then
            bad "the body of '$sym' in $elf forms $pcrel link-time address(es). Every datum
  this body touches above one kernel core is per core, so a link-time address here is a cell
  two cores write: one core's close then subtracts another core's open, which wraps near 2^32
  whenever the peer opened later and reads as a plausible switch cost otherwise"
        fi

        # The open and the close each reach it, so one read is half a bracket. A body reading
        # it zero times is already refused above as unknown, so what reaches here is a body
        # that addresses its stamp and does it per core on one side only.
        if [ "$base" -lt 2 ]; then
            bad "the body of '$sym' in $elf reads the per-CPU base $base time(s), where the
  bracket's open and its close each owe one, so one half of the bracket reaches its stamp some
  other way"
        fi
        continue
    fi

    percore="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
    shared="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
    require_number "$percore" "the indexed stamp access count in $sym"
    require_number "$shared" "the bare-base stamp access count in $sym"
    echo "   $sym: $total instruction(s), $percore indexed stamp access(es), $shared off a
  bare link-time base, $SYNCED_N re-decode(s)"

    if [ "$shared" -ne 0 ]; then
        bad "the body of '$sym' in $elf reaches a stamp cell $shared time(s) off a link-time
  base with no identity read and no scaled add between. Every core lands on that one word, so
  one core's close subtracts another core's open, which wraps near 2^32 whenever the peer
  opened later and reads as a plausible switch cost otherwise"
    fi

    if [ "$percore" -lt 1 ]; then
        bad "the body of '$sym' in $elf names a stamp cell and never dereferences it across
  its $total instruction(s), so this half of the bracket reaches its stamp some way this gate
  carries no model of"
    fi
done

if [ "$rc" -ne 0 ]; then
    exit 1
fi

if [ "$MODE" = base ]; then
    echo "PASS: $SYMS stamps its switch window off the per-CPU base and forms no link-time
  address"
    exit 0
fi

echo "PASS: every stamp access in $SYMS reaches its cell through a link-time base this core's
  identity indexed"
exit 0
