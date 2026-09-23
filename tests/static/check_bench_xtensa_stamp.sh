#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The LX6 switch bracket's END stamp, read out of the LINKED IMAGE: the cell must be CONSUMED
# by the read that banks a sample, and re-stamped on the cooperative resume alone.
#
# TWO BODIES, because the bracket's two halves are not in one. xtensa_switch banks the
# PREVIOUS switch's cost on its way in, the windowed exit below it being unable to host a call.
# The close stands PAST the retw, in arch_switch, so the windowed return and the underflow
# that reloads the incoming frame are inside the measured span; a stamp back at the exit would
# silently shorten the window to the save and the swap. So xtensa_switch owes the consuming
# read and NO stamp, and arch_switch owes the stamp.
#
# WHAT THE DEFECT LOOKS LIKE. A thread resumed through the interrupt frame never returns to
# arch_switch, so the end cell still holds a stamp OLDER than the start written a few
# instructions below the entry; the next entry subtracts the two and banks a delta near 2^32
# as a switch cost. The counter here is 32 bits, so nothing saturates and no column says so:
# the row reports it as an ordinary sample.
#
# WHY THIS IS STRUCTURAL AND NOT A BOUND ON THE SAMPLES. There is no LX6 emulator in this
# tree, so no run anywhere can read that row.
#
# REFUSED: a body whose bank call reads the end cell without clearing it (the defect), one
# that banks without reading it at all, a resume path that never re-stamps the cell, an exit
# that stamps it after all, and a body this reader cannot decode.
#
# THE WINDOW IS FROM THE FIRST MENTION OF THE END CELL TO THE BANK CALL, and the two landmarks
# are how this gate tells a body that does the wrong thing from one it cannot see. A body
# naming neither is a forwarder, one naming the cell and never reaching the bank has lost its
# bracket, and one reaching the bank without ever naming the cell forms the address some way
# this reader carries no model of. Each is UNKNOWN, and each refusal comes from
# tests/lib/objdump_window.awk rather than from an arm here.
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
SYM_CLOSE=arch_switch
END_CELL=g_bench_sw_end
BANK=kickos_bench_switch_done

[ -f "$elf" ] || fail "no image at $elf"
[ -n "$objdump" ] || fail "no objdump given: the objdump argument was empty, so there is no
  instruction stream to decode. CMAKE_OBJDUMP resolved to nothing for this toolchain"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Emits one record: whether the bank's arm READ the end cell, whether it CLEARED it before
# banking, and how many times the cell is re-stamped from a live value.
#
# The literal pool is resolved by the disassembler, which prints the symbol it points at, so
# the end cell is named in the instruction stream and needs no symbol table of its own.
#
# ABOVE ONE KERNEL CORE THE CELL IS AN ARRAY AND THE POOLED ADDRESS IS ITS BASE, so the
# register the pool loaded is scaled-added to this core's index before anything touches it. A
# reader that treated that add as an ordinary write would lose the cell one instruction after
# finding it and report a body that banks against nothing. The add carries the tracking to its
# destination instead; WHETHER the index names a core is
# tests/static/check_bench_stamp_percore.sh's claim and not this one's.
#
# A window rotation and a call4 both rename every register this reader is tracking, so both
# drop the whole tracking state rather than carrying a name across.
#
# HALF A PROGRAM: `seen`, the body scope and every refusal above come from gate.sh's
# scoped_body, which reads tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead
# of this file.
cat > "$TMP/reader.awk" <<'AWK'
# The bank call is the window's closing landmark, so the snapshot is taken on the instruction
# the shared window named and not on a second search for the same callee.
win_n == win_close_n { bankread = readcell; bankclear = clearcell }
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
    if (mn ~ /^add(x[248])?([.]n)?$/ && nops == 3)
    {
        if (endreg != "" && (o[2] == endreg || o[3] == endreg))
        {
            endreg = o[1]
            delete zero[o[1]]
            next
        }
    }
    if (mn ~ /^call[0-9]+$/ || mn ~ /^callx[0-9]+$/)
    {
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
END { printf "REC %d %d %d\n", bankread + 0, bankclear + 0, stamps + 0 }
AWK

read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v cell_re="<$END_CELL>" -v win_open="<$END_CELL>" -v win_close="<$BANK>"
}

# The close body carries no bank call, so the window runs from the cell's first mention to the
# end of the body: "from X onwards". A body that never names the cell is still a refusal, which
# is what keeps a stamp moved out of arch_switch from reading as a stamp count of zero.
read_close_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v cell_re="<$END_CELL>" -v win_open="<$END_CELL>" -v win_close=""
}

# --- the reader's controls, before the image is read --------------------------
# Planted in the shape the invocation below produces, which is --no-show-raw-insn.
plant() { # <clearing?> <restamping?>
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
    if [ "$2" = restamping ]; then
        echo "    101d:	rsr.ccount	a5"
        echo "    1020:	l32r	a6, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
        echo "    1023:	s32i.n	a5, a6, 0"
    fi
    echo "    1025:	retw.n"
}

# The close body: the resume side, which names the cell and writes a live count into it.
plant_close() { # <stamping?>
    echo '00001000 <planted_close>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	call8	3000 <planted_switch>"
    if [ "$1" = stamping ]; then
        echo "    1007:	rsr.ccount	a9"
        echo "    100a:	l32r	a8, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
        echo "    100d:	memw"
        echo "    1010:	s32i.n	a9, a8, 0"
    fi
    echo "    1012:	retw.n"
}

plant clearing bare        > "$TMP/ctl_consumed"
plant stale    bare        > "$TMP/ctl_stale"
plant clearing restamping  > "$TMP/ctl_exit_stamp"
plant_close stamping       > "$TMP/ctl_close"
plant_close bare           > "$TMP/ctl_close_nostamp"

{
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	retw.n"
} > "$TMP/ctl_nobench"

{
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	l32r	a6, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
    echo "    1007:	l32i	a4, a6, 0"
    echo "    100a:	retw.n"
} > "$TMP/ctl_nobank"

{
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	call4	2000 <$BANK>"
    echo "    1007:	retw.n"
} > "$TMP/ctl_nocell"

echo '00001000 <planted_switch>:' > "$TMP/ctl_noinsn"

ctl="$(read_body "$TMP/ctl_consumed" planted_switch)"
case "$ctl" in
    "REC 1 1 0") ;;
    *) fail "the reader answered [$ctl] for a planted entry body that consumes its end stamp
  before banking and leaves the re-stamp to the resume side, so it cannot recognise the shape
  this gate requires and every verdict below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_exit_stamp" planted_switch)"
case "$ctl" in
    "REC 1 1 1") ;;
    *) fail "the reader answered [$ctl] for a planted entry body that re-stamps the end cell
  itself, which is the shape that shortens the window back to the save and the swap. A reader
  that does not count that stamp cannot go red on it" ;;
esac

ctl="$(read_close_body "$TMP/ctl_close" planted_close)"
case "$ctl" in
    "REC 0 0 1") ;;
    *) fail "the reader answered [$ctl] for a planted resume body that writes a live count
  into the end cell, so it cannot see the half of the bracket that stands past the retw" ;;
esac

ctl="$(read_close_body "$TMP/ctl_close_nostamp" planted_close)"
case "$ctl" in
    "NOOPEN 3") ;;
    *) fail "the reader answered [$ctl] for a planted resume body that never names the end
  cell. Read as a record it reports a resume that stamps nothing, which is a finding about the
  image; what it is, is a close this gate could not see" ;;
esac

ctl="$(read_body "$TMP/ctl_stale" planted_switch)"
case "$ctl" in
    "REC 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that banks a delta against an end
  stamp it never cleared. That is the defect this gate exists to catch, so a reader that does
  not report it cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_nobench" planted_switch)"
case "$ctl" in
    "NOBRACKET 2") ;;
    *) fail "the reader answered [$ctl] for a planted two-instruction body naming neither the
  end cell nor the bank call. Read as a record it reports an image that banks nothing, which is
  a finding about the image; what it is, is a switch body this gate could not see" ;;
esac

ctl="$(read_body "$TMP/ctl_nobank" planted_switch)"
case "$ctl" in
    "NOCLOSE 4") ;;
    *) fail "the reader answered [$ctl] for a planted body that names the end cell and never
  reaches the bank call. The window then runs to the end of the body and every count in it
  describes a bracket this gate never found" ;;
esac

ctl="$(read_body "$TMP/ctl_nocell" planted_switch)"
case "$ctl" in
    "NOOPEN 3") ;;
    *) fail "the reader answered [$ctl] for a planted body that banks without ever naming the
  end cell. The literal pool is how that cell is named here, so a body forming its address any
  other way is UNKNOWN and not a body that banks against nothing" ;;
esac

ctl="$(read_body "$TMP/ctl_noinsn" planted_switch)"
case "$ctl" in
    NOINSN) ;;
    *) fail "the reader answered [$ctl] for a body header carrying no instruction at all, so a
  listing this disassembler could not decode would emit a zeroed record and read as an image
  that banks no switch cost" ;;
esac

# The same shapes ABOVE ONE KERNEL CORE, where the pooled address is an array base and this
# core's index reaches the slot. Every record below is the one-core record: what the scaled add
# changes is whether this reader can still SEE the cell one instruction after the pool loaded
# it, and a reader that loses it there reports an image that banks against nothing.
plant_indexed() { # <clearing?>
    echo '00001000 <planted_switch>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	l32r	a6, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
    echo "    1007:	rsr.prid	a5"
    echo "    100a:	extui	a5, a5, 1, 1"
    echo "    100d:	addx4	a6, a5, a6"
    echo "    1010:	l32i	a4, a6, 0"
    echo "    1013:	beqz	a4, 1030 <planted_switch+0x30>"
    if [ "$1" = clearing ]; then
        echo "    1016:	movi	a7, 0"
        echo "    1019:	s32i.n	a7, a6, 0"
    fi
    echo "    101b:	l32r	a6, 1104 <pool+0x4> (3ffb3b1c <g_bench_sw_start>)"
    echo "    101e:	rsr.prid	a5"
    echo "    1021:	extui	a5, a5, 1, 1"
    echo "    1024:	addx4	a6, a5, a6"
    echo "    1027:	l32i.n	a6, a6, 0"
    echo "    1029:	sub	a6, a4, a6"
    echo "    102c:	call4	2000 <$BANK>"
    echo "    102f:	retw.n"
}

# The resume side's index comes out of a CALL, the windowed ABI returning it in a2 rotated by
# the call's width.
plant_close_indexed() { # <stamping?>
    echo '00001000 <planted_close>:'
    echo "    1000:	entry	a1, 32"
    echo "    1004:	call8	3000 <arch_cpu_id>"
    echo "    1007:	rsr.ccount	a9"
    echo "    100a:	l32r	a8, 1100 <pool+0x0> (3ffb3b18 <$END_CELL>)"
    echo "    100d:	addx4	a10, a10, a8"
    if [ "$1" = stamping ]; then
        echo "    1010:	s32i.n	a9, a10, 0"
    fi
    echo "    1012:	retw.n"
}

plant_indexed clearing       > "$TMP/ctl_ix_consumed"
plant_indexed stale          > "$TMP/ctl_ix_stale"
plant_close_indexed stamping > "$TMP/ctl_ix_close"
plant_close_indexed bare     > "$TMP/ctl_ix_close_nostore"

ctl="$(read_body "$TMP/ctl_ix_consumed" planted_switch)"
case "$ctl" in
    "REC 1 1 0") ;;
    *) fail "the reader answered [$ctl] for a planted entry body that reaches its end stamp
  through a scaled add on the pooled base, which is the shape above one kernel core. Read as a
  record it reports a bank that never touched the cell, so a correct image goes red" ;;
esac

ctl="$(read_body "$TMP/ctl_ix_stale" planted_switch)"
case "$ctl" in
    "REC 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted entry body that reaches its end stamp
  through a scaled add and banks against it without clearing it. That is the defect this gate
  exists to catch, in the shape the two-core image takes, so a reader that does not report it
  cannot go red" ;;
esac

ctl="$(read_close_body "$TMP/ctl_ix_close" planted_close)"
case "$ctl" in
    "REC 0 0 1") ;;
    *) fail "the reader answered [$ctl] for a planted resume body that writes a live count into
  its own core's slot of the end cell, so it cannot see the half of the bracket that stands
  past the retw once that cell is an array" ;;
esac

ctl="$(read_close_body "$TMP/ctl_ix_close_nostore" planted_close)"
case "$ctl" in
    "REC 0 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted resume body that indexes the end cell and
  then writes nothing to it. That is the shape that leaves the bank in '$SYM' reading a cell
  only ever cleared, so a reader counting the scaled add as the stamp cannot go red on it" ;;
esac

ctl_dead_reader "$(read_body "$TMP/ctl_consumed" a_symbol_no_listing_carries)" \
    "a renamed switch body would read as a clean one"

ctl_dead_reader "$(read_close_body "$TMP/ctl_close" a_symbol_no_listing_carries)" \
    "a renamed resume body would read as one that stamps nothing"

synced_body_control

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the LX6 switch bracket's end stamp in $elf =="

# A linear sweep of a variable-width listing decodes forward from wherever it last stopped, and
# the esp32 link leaves two zero bytes behind a relaxed jump. Everything behind one of those is
# instructions that were never in the image until the stream realigns, which reads as a body
# that banks nothing. BRANCH names the mnemonics whose operand is an instruction boundary, and
# gate.sh's synced_body re-decodes from the ones this listing did not carry.
BRANCH='^(b[a-z]*([.]n)?|j|loop[a-z]*)$'

synced_body "$TMP/body" "$TMP/dis" "$SYM" "$elf" "$objdump" "$BRANCH"
entry_synced="$SYNCED_N"
rec="$(read_body "$TMP/body" "$SYM")"
case "$rec" in
    NOSYM)
        fail "the disassembly of $elf carries no body for '$SYM', so the reader started
  nowhere. The switch body was renamed, or the disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    "NOBRACKET "*)
        fail "the body of '$SYM' in $elf names NEITHER $END_CELL nor '$BANK' across its
  $(printf '%s\n' "$rec" | cut -d' ' -f2) instruction(s). This gate cannot see the bracket it
  reads, so what it has is UNKNOWN and not an image that banks nothing: the symbol names a
  forwarder, or both were renamed. Point it at the body that carries them" ;;
    "NOOPEN "*)
        fail "the body of '$SYM' in $elf reaches '$BANK' without ever naming $END_CELL across
  its $(printf '%s\n' "$rec" | cut -d' ' -f2) instruction(s). The literal pool is how that cell
  is named in this listing, so a body addressing it any other way is UNKNOWN and not a body
  that banks a delta against nothing" ;;
    "NOCLOSE "*)
        fail "the body of '$SYM' in $elf names $END_CELL and never reaches '$BANK' across its
  $(printf '%s\n' "$rec" | cut -d' ' -f2) instruction(s), so the window this rule is stated
  over never closes. That is UNKNOWN, not an image that banks no switch cost: the bracket was
  deleted, or this gate is reading a body that cannot carry the defect" ;;
    "REC "*) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

bankread="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
bankclear="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
stamps="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
require_number "$bankread" "the end-cell read ahead of the bank in $SYM"
require_number "$bankclear" "the end-cell clear ahead of the bank in $SYM"
require_number "$stamps" "the end-cell stamp count in $SYM"
synced_body "$TMP/body_close" "$TMP/dis" "$SYM_CLOSE" "$elf" "$objdump" "$BRANCH"
rec="$(read_close_body "$TMP/body_close" "$SYM_CLOSE")"
case "$rec" in
    NOSYM)
        fail "the disassembly of $elf carries no body for '$SYM_CLOSE', so the half of the
  bracket that stands past the retw was read nowhere. The resume body was renamed or inlined
  away" ;;
    NOINSN)
        fail "the body of '$SYM_CLOSE' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
    "NOOPEN "*)
        fail "the body of '$SYM_CLOSE' in $elf never names $END_CELL across its
  $(printf '%s\n' "$rec" | cut -d' ' -f2) instruction(s). The literal pool is how that cell is
  named in this listing, so a body addressing it any other way is UNKNOWN and not a resume that
  stamps nothing: the close moved, or it is back at the windowed exit where it shortens the
  window to the save and the swap" ;;
    "REC "*) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM_CLOSE', a record this gate does not model" ;;
esac

closestamps="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
require_number "$closestamps" "the end-cell stamp count in $SYM_CLOSE"

echo "   corpus: '$SYM' reads $END_CELL $bankread time(s) and clears it $bankclear time(s)
  ahead of the bank call and re-stamps it $stamps time(s); '$SYM_CLOSE' stamps it
  $closestamps time(s); $entry_synced and $SYNCED_N re-decode(s)"

rc=0

if [ "$bankread" -eq 0 ]; then
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

if [ "$stamps" -ne 0 ]; then
    bad "the body of '$SYM' in $elf writes a live value to $END_CELL $stamps time(s). The
  close belongs past the retw, in '$SYM_CLOSE': stamped at the windowed exit instead, the
  window ends before the ps write and the underflow that reloads the incoming frame, and the
  row silently becomes the save and the swap alone"
fi

if [ "$closestamps" -eq 0 ]; then
    bad "the body of '$SYM_CLOSE' in $elf never writes a live value to $END_CELL, so the cell
  the bank in '$SYM' reads is only ever cleared and this image's switch row can take no sample"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: '$SYM' consumes $END_CELL on the read that banks and stamps it nowhere, and
  '$SYM_CLOSE' stamps it $closestamps time(s) past the retw"
exit 0
