#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The LX6 switch bracket's END stamp, read out of the LINKED IMAGE: the cell must be CONSUMED
# by the read that banks a sample, and re-stamped on the cooperative exit alone.
#
# WHAT THE DEFECT LOOKS LIKE. xtensa_switch banks the PREVIOUS switch's cost on its way in,
# because the windowed exit below it cannot host a call. A thread resumed through the
# interrupt frame leaves that exit unrun, so the end cell still holds a stamp OLDER than the
# start written a few instructions further down; the next entry subtracts the two and banks a
# delta near 2^32 as a switch cost. The counter here is 32 bits, so nothing saturates and no
# column says so: the row reports it as an ordinary sample.
#
# WHY THIS IS STRUCTURAL AND NOT A BOUND ON THE SAMPLES. There is no LX6 emulator in this
# tree, so no run anywhere can read that row.
#
# REFUSED: a body whose bank call reads the end cell without clearing it (the defect), one
# that banks without reading it at all, one that never re-stamps the cell, one carrying no
# bank call, and a body this reader cannot decode.
#
# usage: check_bench_xtensa_stamp.sh <elf> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_xtensa_stamp.sh <elf> <objdump>"
elf="${1:?$_usage}"
# CMAKE_OBJDUMP resolves to the empty string when CMake's binutils detection finds no objdump
# for a toolchain, and that empty argument must be refused as a missing disassembler, not as a
# missing invocation argument.
objdump="${2:-}"

SYM=xtensa_switch
END_CELL=g_bench_sw_end
BANK=kickos_bench_switch_done

[ -f "$elf" ] || fail "no image at $elf"
[ -n "$objdump" ] || fail "no objdump given: the objdump argument was empty, so there is no
  instruction stream to decode. CMAKE_OBJDUMP resolved to nothing for this toolchain"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Emits one record: whether the body banks, whether the bank's arm READ the end cell, whether
# it CLEARED it before banking, and how many times the cell is re-stamped from a live value.
#
# The literal pool is resolved by the disassembler, which prints the symbol it points at, so
# the end cell is named in the instruction stream and needs no symbol table of its own.
#
# A window rotation and a call4 both rename every register this reader is tracking, so both
# drop the whole tracking state rather than carrying a name across.
#
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk ahead of this file.
cat > "$TMP/reader.awk" <<'AWK'
function forget(    r)
{
    endreg = ""
    for (r in zero) { delete zero[r] }
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

    if (mn == "l32r" && nops == 2 && o[2] ~ cell_re)
    {
        endreg = o[1]
        delete zero[o[1]]
        next
    }
    if (mn ~ /^movi(\.n)?$/ && nops == 2)
    {
        if (o[1] == endreg) { endreg = "" }
        if (o[2] == "0") { zero[o[1]] = 1 } else { delete zero[o[1]] }
        next
    }
    if (mn ~ /^l32i(\.n)?$/ && nops == 3)
    {
        if (endreg != "" && o[2] == endreg && o[3] == "0") { readcell = 1 }
        if (o[1] == endreg) { endreg = "" }
        delete zero[o[1]]
        next
    }
    if (mn ~ /^s32i(\.n)?$/ && nops == 3)
    {
        if (endreg != "" && o[2] == endreg && o[3] == "0")
        {
            if (o[1] in zero) { clearcell = 1 } else { stamps++ }
        }
        next
    }
    if (mn ~ /^call[0-9]+$/ || mn ~ /^callx[0-9]+$/)
    {
        if (mn ~ /^call[0-9]+$/ && ops ~ bank_re && bank == 0)
        {
            bank = 1
            bankread = readcell
            bankclear = clearcell
        }
        forget()
        next
    }
    if (mn == "rotw")
    {
        forget()
        next
    }
    # Anything else: a first operand naming a tracked register is a write to it.
    if (nops >= 1)
    {
        if (o[1] == endreg) { endreg = "" }
        delete zero[o[1]]
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    printf "REC %d %d %d %d\n", bank + 0, bankread + 0, bankclear + 0, stamps + 0
}
AWK

read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v cell_re="<$END_CELL>" -v bank_re="<$BANK>"
}

# --- the reader's controls, before the image is read --------------------------
# Planted in the shape the invocation below produces, which is --no-show-raw-insn.
plant() { # <clearing?>
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	l32r	a6, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
    echo "    1007:	l32i	a4, a6, 0"
    echo "    100a:	beqz	a4, 1020 <planted_switch+0x20>"
    if [ "$1" = clearing ]; then
        echo "    100d:	movi	a7, 0"
        echo "    1010:	s32i.n	a7, a6, 0"
    fi
    echo "    1012:	l32r	a6, 1104 <pool+0x4> (3ffb3b1c <g_bench_sw_start>)"
    echo "    1015:	l32i.n	a6, a6, 0"
    echo "    1017:	sub	a6, a4, a6"
    echo "    101a:	call4	2000 <$BANK>"
    echo "    101d:	rsr.ccount	a5"
    echo "    1020:	l32r	a6, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
    echo "    1023:	s32i.n	a5, a6, 0"
    echo "    1025:	retw.n"
}

plant clearing > "$TMP/ctl_consumed"
plant stale    > "$TMP/ctl_stale"

{
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	retw.n"
} > "$TMP/ctl_nobench"

ctl="$(read_body "$TMP/ctl_consumed" planted_switch)"
case "$ctl" in
    "REC 1 1 1 1") ;;
    *) fail "the reader answered [$ctl] for a planted body that consumes its end stamp before
  banking and re-stamps it once, so it cannot recognise the shape this gate requires and every
  verdict below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_stale" planted_switch)"
case "$ctl" in
    "REC 1 1 0 1") ;;
    *) fail "the reader answered [$ctl] for a planted body that banks a delta against an end
  stamp it never cleared. That is the defect this gate exists to catch, so a reader that does
  not report it cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_nobench" planted_switch)"
case "$ctl" in
    "REC 0 0 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying no bracket at all, so an
  image whose stamping was deleted would not be reported as such" ;;
esac

ctl_dead_reader "$(read_body "$TMP/ctl_consumed" a_symbol_no_listing_carries)" \
    "a renamed switch body would read as a clean one"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the LX6 switch bracket's end stamp in $elf =="

rec="$(read_body "$TMP/dis" "$SYM")"
case "$rec" in
    NOSYM)
        fail "the disassembly of $elf carries no body for '$SYM', so the reader started
  nowhere. The switch body was renamed, or the disassembler's output shape has moved" ;;
    "REC "*) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

bank="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
bankread="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
bankclear="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
stamps="$(printf '%s\n' "$rec" | cut -d' ' -f5)"
require_number "$bank" "the bank-call count in $SYM"
require_number "$bankread" "the end-cell read ahead of the bank in $SYM"
require_number "$bankclear" "the end-cell clear ahead of the bank in $SYM"
require_number "$stamps" "the end-cell stamp count in $SYM"
echo "   corpus: '$SYM' banks $bank time(s), reads $END_CELL $bankread time(s) and clears it
  $bankclear time(s) ahead of that call, and re-stamps it $stamps time(s)"

rc=0

if [ "$bank" -eq 0 ]; then
    bad "the body of '$SYM' in $elf calls '$BANK' nowhere, so this image banks no switch cost
  at all and the row it publishes describes nothing. Either the bracket was deleted or this
  gate is reading a body that cannot carry the defect"
elif [ "$bankread" -eq 0 ]; then
    bad "the body of '$SYM' in $elf reaches '$BANK' without ever reading $END_CELL, so the
  delta it banks is formed from something this gate carries no model of and the claim below
  cannot be made either way"
elif [ "$bankclear" -eq 0 ]; then
    bad "the body of '$SYM' in $elf banks a delta against $END_CELL and never clears it. Only
  the cooperative exit refreshes that cell, so a switch resuming a thread through the interrupt
  frame leaves a stamp OLDER than the start written below it, and the next entry banks
  end - start as a switch cost near 2^32. This counter is 32 bits, so no SAT column refuses it
  and the row publishes it as an ordinary sample"
fi

if [ "$stamps" -eq 0 ]; then
    bad "the body of '$SYM' in $elf never writes a live value to $END_CELL, so the cell the
  bank above reads is only ever cleared and this image's switch row can take no sample"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: '$SYM' consumes $END_CELL on the read that banks, and re-stamps it $stamps time(s)"
exit 0
