#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The armv8a bench cycle source's PMU programming, read out of the LINKED IMAGE: the value
# written to PMCR_EL0 must carry E, C and LC, and must NOT carry D. LC puts the cycle
# counter's overflow at bit 63 rather than bit 31; D divides the count by 64.
#
# REFUSED: a body that writes PMCR_EL0 without E, C and LC provably set in the value written,
# one that writes it without D provably clear, one that writes it more than once, a body
# carrying any branch but a terminal unconditional one, and a body this reader cannot decode.
#
# A BODY THAT NEVER WRITES PMCR_EL0 IS UNKNOWN AND NOT A FINDING. The write is the window's
# opening landmark, so a two-instruction tail-call thunk over the real per-core init passes the
# branch pre-check below and would otherwise be reported as programming the PMU zero times.
# tests/lib/objdump_window.awk refuses it by name instead.
#
# WHY THIS IS STATIC AND NOT A RUNTIME ARM. QEMU makes LC writable and returns PMCCNTR_EL0 at
# its full 64 bits whatever LC holds, so an image that clears the bit reports the same
# distribution, the same sat-probe and the same rate line as one that sets it, on the only
# armv8a vehicle this tree has.
#
# D is the same story one bit over: losing it divides every count by 64 and the figures still
# come out as a plausible distribution, so no vehicle here tells that image apart from a
# correct one either.
#
# usage: check_bench_a53_pmcr.sh <elf> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_a53_pmcr.sh <elf> <objdump> <arch>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
arch="${3:?$_usage}"

SYM=kickos_armv8a_percore_init
# E (bit 0), C (bit 2) and LC (bit 6), as arch/arm64/common/arch_arm64_a53.cc ORs in from one
# immediate; D (bit 3) is the bit that same body clears first.
# The opening landmark, as an ERE over the text tests/lib/objdump_window.awk normalises.
W_PMCR='^msr pmcr_el0,'

case "$arch" in
    armv8a) ;;
    *)
        fail "arch '$arch' was handed to a gate that models the armv8a PMU alone; PMCR_EL0 is
  not a register it has. The registration in tests/integration/gates/bench.cmake decides who
  runs this, so a refusal here means that guard moved" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Emits one record: the body's instruction count, its PMCR_EL0 writes, the writes whose value
# carries E, C and LC each PROVABLY SET, and the writes whose value carries D PROVABLY CLEAR.
#
# THE STATE IS PER BIT, NOT PER REGISTER. Each of the four tracked registers-of-interest bits
# (E, C, D, LC) sits in one of three states per GPR: known-1, known-0, or unknown. `mov`
# with an immediate sets all four from the literal; `orr`/`and` with a register or an
# immediate combine two such states bitwise; every other destination-writing instruction
# (mrs included, since its source is a system register the reader does not model) loses the
# claim for that register outright.
#
# STRAIGHT-LINE, AND THE READER COUNTS WHAT WOULD MAKE IT OTHERWISE. The state is carried
# forward instruction by instruction with no second path modelled, so a conditional branch over
# the setup leaves the verdict describing the path this reader walked while the other one writes
# whatever `mrs` returned, one write either way, which the write-count arm cannot see. A call
# is the same hole one step over: it returns to the next instruction and clobbers x0-x18, which
# this reader does not model. Only an unconditional branch as the body's LAST instruction is
# analysable, the tail call the compiler emits for this body's own final call; `brmid` counts
# every other branch and `brcond` every conditional one, and the gate refuses on either.
# HALF A PROGRAM: `seen`, the body scope and the refusal above come from gate.sh's scoped_body,
# which reads tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead of this file.
cat > "$TMP/reader.awk" <<'AWK'
function regnum(t,   s) {
    s = t
    gsub(/[^0-9a-zA-Z]/, "", s)
    if (s !~ /^[wx][0-9]+$/) { return -1 }
    return substr(s, 2) + 0
}
function trim(t,   s) {
    s = t
    sub(/^[ \t]+/, "", s)
    sub(/[ \t]+$/, "", s)
    return s
}
function hexval(c) {
    if (c == "0") { return 0 }
    if (c == "1") { return 1 }
    if (c == "2") { return 2 }
    if (c == "3") { return 3 }
    if (c == "4") { return 4 }
    if (c == "5") { return 5 }
    if (c == "6") { return 6 }
    if (c == "7") { return 7 }
    if (c == "8") { return 8 }
    if (c == "9") { return 9 }
    if (c == "a") { return 10 }
    if (c == "b") { return 11 }
    if (c == "c") { return 12 }
    if (c == "d") { return 13 }
    if (c == "e") { return 14 }
    return 15
}
function nibble_bit(v, i,   h) {
    h = int(v / (2 ^ i))
    return h % 2
}
# BIT_E/BIT_C/BIT_D/BIT_LC: out-parameters, since this awk has no other cheap way to hand back
# four values. Bit 6 (LC) lives in the SECOND hex digit from the right (nibble 1's own bit 2):
# 0x45's nibble 1 is '4' = 0100, whose bit 2 is the 1 that makes LC.
function decode_imm(str,    hx, len, n0, n1, v0, v1) {
    hx = str
    sub(/^#0x/, "", hx)
    len = length(hx)
    n0 = substr(hx, len, 1)
    v0 = hexval(n0)
    if (len >= 2) {
        n1 = substr(hx, len - 1, 1)
        v1 = hexval(n1)
    } else {
        v1 = 0
    }
    if (nibble_bit(v0, 0)) { BIT_E = "1" } else { BIT_E = "0" }
    if (nibble_bit(v0, 2)) { BIT_C = "1" } else { BIT_C = "0" }
    if (nibble_bit(v0, 3)) { BIT_D = "1" } else { BIT_D = "0" }
    if (nibble_bit(v1, 2)) { BIT_LC = "1" } else { BIT_LC = "0" }
}
function is_cond_branch(m) {
    if (m ~ /^b\./) { return 1 }
    if (m ~ /^cbz$|^cbnz$|^tbz$|^tbnz$/) { return 1 }
    return 0
}
function is_branch(m) {
    if (is_cond_branch(m)) { return 1 }
    if (m ~ /^b$|^bl$|^br$|^blr$|^braa$|^brab$|^blraa$|^blrab$/) { return 1 }
    if (m ~ /^ret$|^retaa$|^retab$/) { return 1 }
    return 0
}
function is1(s) { return s == "1" }
function is0(s) { return s == "0" }
# Three states per bit (known-1 / known-0 / unknown), not a taint boolean: combining an
# UNKNOWN bit with a KNOWN one still resolves in the direction the known operand forces, and
# only two unknowns (or two disagreeing knowns) stay unknown.
function or_bit(a, b) {
    if (is1(a) || is1(b)) { return "1" }
    if (is0(a) && is0(b)) { return "0" }
    return "U"
}
function and_bit(a, b) {
    if (is0(a) || is0(b)) { return "0" }
    if (is1(a) && is1(b)) { return "1" }
    return "U"
}
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    sub(/[ \t]*\/\/.*$/, "", text)
    n++
    mn = text
    sub(/[ \t].*$/, "", mn)
    ops = text
    if (ops ~ /[ \t]/) { sub(/^[^ \t]+[ \t]*/, "", ops) } else { ops = "" }
    nf = split(ops, o, ",")
    for (i = 1; i <= nf; i++) { o[i] = trim(o[i]) }

    if (is_branch(mn)) {
        brtot++
        brlast = n
        if (is_cond_branch(mn)) { brcond++ }
        next
    }

    if (mn == "msr") {
        if (o[1] == "pmcr_el0") {
            wr++
            r = regnum(o[2])
            if (r >= 0) {
                if (is1(sE[r]) && is1(sC[r]) && is1(sLC[r])) { wrbits++ }
                if (is0(sD[r])) { wrdclear++ }
            }
        }
        next
    }

    if (nf < 1) { next }
    d = regnum(o[1])
    if (d < 0) { next }

    if (mn == "mov" && nf == 2) {
        s = regnum(o[2])
        if (s >= 0) {
            sE[d] = sE[s]; sC[d] = sC[s]; sD[d] = sD[s]; sLC[d] = sLC[s]
        } else if (o[2] ~ /^#0x[0-9a-f]+$/) {
            decode_imm(o[2])
            sE[d] = BIT_E; sC[d] = BIT_C; sD[d] = BIT_D; sLC[d] = BIT_LC
        } else {
            sE[d] = "U"; sC[d] = "U"; sD[d] = "U"; sLC[d] = "U"
        }
        next
    }

    if ((mn == "orr" || mn == "and") && nf == 3) {
        s1 = regnum(o[2])
        if (s1 >= 0) {
            e1 = sE[s1]; c1 = sC[s1]; d1 = sD[s1]; lc1 = sLC[s1]
        } else {
            e1 = "U"; c1 = "U"; d1 = "U"; lc1 = "U"
        }
        if (o[3] ~ /^#0x[0-9a-f]+$/) {
            decode_imm(o[3])
            e2 = BIT_E; c2 = BIT_C; d2 = BIT_D; lc2 = BIT_LC
        } else {
            s2 = regnum(o[3])
            if (s2 >= 0) {
                e2 = sE[s2]; c2 = sC[s2]; d2 = sD[s2]; lc2 = sLC[s2]
            } else {
                e2 = "U"; c2 = "U"; d2 = "U"; lc2 = "U"
            }
        }
        if (mn == "orr") {
            sE[d] = or_bit(e1, e2); sC[d] = or_bit(c1, c2)
            sD[d] = or_bit(d1, d2); sLC[d] = or_bit(lc1, lc2)
        } else {
            sE[d] = and_bit(e1, e2); sC[d] = and_bit(c1, c2)
            sD[d] = and_bit(d1, d2); sLC[d] = and_bit(lc1, lc2)
        }
        next
    }

    # Every other destination-writing mnemonic loses the claim for that register outright
    # rather than keep a state this reader did not derive.
    sE[d] = "U"; sC[d] = "U"; sD[d] = "U"; sLC[d] = "U"
}
END {
    brmid = brtot + 0
    if (brtot > 0 && brlast == n) { brmid = brtot - 1 }
    printf "COUNTS %d %d %d %d %d %d\n", n, wr + 0, wrbits + 0, wrdclear + 0, brmid, brcond + 0
}
AWK

read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v win_open="$W_PMCR"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn.
# The objdump this gate runs prints a decimal gloss after a MOVZ immediate ("// #69") and
# none after a logical-immediate one (AND/ORR with #imm), so the plants carry that split.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x45                   	// #69"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	msr	pmcr_el0, x0"
} > "$TMP/ctl_lc"

# THE DEFECT: E and C set, D cleared, LC absent.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x5                   	// #5"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	msr	pmcr_el0, x0"
} > "$TMP/ctl_nolc"

# The immediate present and the write gone: a body that computes the value and programs
# nothing leaves the PMU at its reset state, which is where D is UNKNOWN.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mov	x1, #0x45                   	// #69"
    echo "    1004:	nop"
} > "$TMP/ctl_nowrite"

# THE DECOY: 0x45 reaches a register nothing programs, while PMCR_EL0 is written from a value
# with LC clear.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mov	x1, #0x45                   	// #69"
    echo "    1004:	mrs	x0, pmcr_el0"
    echo "    1008:	and	x0, x0, #0xfffffffffffffff7"
    echo "    100c:	mov	x2, #0x5                   	// #5"
    echo "    1010:	orr	x0, x0, x2"
    echo "    1014:	msr	pmcr_el0, x0"
} > "$TMP/ctl_decoy"

# PLANT A: the OR sets LC, then a later AND clears bit 6 alone.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x45                   	// #69"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	and	x0, x0, #0xffffffffffffffbf"
    echo "    1014:	msr	pmcr_el0, x0"
} > "$TMP/ctl_lc_cleared"

# PLANT B: E, C and LC all still reach the write, but a later OR sets D (bit 3) as well.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x45                   	// #69"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	orr	x0, x0, #0x8                   	// #8"
    echo "    1014:	msr	pmcr_el0, x0"
} > "$TMP/ctl_dbit_set"

# THE SHAPE THE COMPILER ACTUALLY EMITS: this body's own last statement is a call, and at -Os it
# leaves the function as an unconditional branch with nothing after it.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x45                   	// #69"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	msr	pmcr_el0, x0"
    echo "    1014:	b	2000 <planted_next>"
} > "$TMP/ctl_tail"

# PLANT C: a conditional branch OVER the setup.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	cbz	x1, 1014 <planted_init+0x14>"
    echo "    1008:	and	x0, x0, #0xfffffffffffffff7"
    echo "    100c:	mov	x1, #0x45                   	// #69"
    echo "    1010:	orr	x0, x0, x1"
    echo "    1014:	msr	pmcr_el0, x0"
} > "$TMP/ctl_branch"

# PLANT D: a call between the OR and the write.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mrs	x0, pmcr_el0"
    echo "    1004:	and	x0, x0, #0xfffffffffffffff7"
    echo "    1008:	mov	x1, #0x45                   	// #69"
    echo "    100c:	orr	x0, x0, x1"
    echo "    1010:	bl	2000 <planted_helper>"
    echo "    1014:	msr	pmcr_el0, x0"
} > "$TMP/ctl_call"

ctl="$(read_body "$TMP/ctl_lc" planted_init)"
case "$ctl" in
    "COUNTS 5 1 1 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that programs PMCR_EL0 with E|C|LC
  and D clear, so it cannot recognise the shape this gate requires and every verdict below is
  meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_nolc" planted_init)"
case "$ctl" in
    "COUNTS 5 1 0 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that programs PMCR_EL0 without LC.
  That is the defect this gate exists to catch, so a reader that does not report it cannot go
  red" ;;
esac

ctl="$(read_body "$TMP/ctl_nowrite" planted_init)"
case "$ctl" in
    "NOOPEN 2") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying the immediate and no write.
  Read as a record it reports the PMU programmed zero times, which is a claim about the image;
  what it is, is a body this gate could not find the write in" ;;
esac

# THE RESIDUAL THIS WINDOW EXISTS FOR: a two-instruction tail-call thunk over the real body. It
# carries one unconditional branch as its last instruction, which the branch pre-check excuses,
# and then reports the PMU as never programmed.
{
    echo '0000000000001000 <planted_init>:'
    echo "    1000:	mov	x0, x19"
    echo "    1004:	b	2000 <planted_init_real>"
} > "$TMP/ctl_thunk"

ctl="$(read_body "$TMP/ctl_thunk" planted_init)"
case "$ctl" in
    "NOOPEN 2") ;;
    *) fail "the reader answered [$ctl] for a planted two-instruction thunk over the real
  per-core init. Its one branch is terminal, so the branch pre-check passes it, and every bit
  arm below then reads zero writes as a PMU nobody programmed" ;;
esac

ctl="$(read_body "$TMP/ctl_decoy" planted_init)"
case "$ctl" in
    "COUNTS 6 1 0 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying the immediate on a register
  the write never reads, beside a PMCR_EL0 write of a value without LC. It reports one write
  exactly as a correct body does unless LC is checked bit-level, so a reader that does not
  separate the two passes the defect" ;;
esac

ctl="$(read_body "$TMP/ctl_lc_cleared" planted_init)"
case "$ctl" in
    "COUNTS 6 1 0 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that ORs in E|C|LC and then ANDs
  LC back out before the write. A register-level taint reports this write as sourced from
  #0x45 same as a correct body, so a reader that does not track the LC bit itself passes the
  defect audit finding 11 exists for" ;;
esac

ctl="$(read_body "$TMP/ctl_dbit_set" planted_init)"
case "$ctl" in
    "COUNTS 6 1 1 0 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that ORs in E|C|LC correctly and
  then sets D on top. E, C and LC all reach the write here, so the LC arm alone cannot catch
  it; a reader without a separate D-clear check passes an image whose bench figures come out
  64x small and plausible" ;;
esac

ctl="$(read_body "$TMP/ctl_tail" planted_init)"
case "$ctl" in
    "COUNTS 6 1 1 1 0 0") ;;
    *) fail "the reader answered [$ctl] for a planted body ending in the tail call the compiler
  emits for this function. A terminal unconditional branch splits no path, and a reader that
  counted it would refuse every real image" ;;
esac

ctl="$(read_body "$TMP/ctl_branch" planted_init)"
case "$ctl" in
    "COUNTS 6 1 1 1 1 1") ;;
    *) fail "the reader answered [$ctl] for a planted body whose conditional branch skips the
  setup. It writes PMCR_EL0 once with E, C and LC set and D clear on the path this reader walks,
  so every other arm here is satisfied by it; only the branch count separates it from a body that
  programs the PMU unconditionally" ;;
esac

ctl="$(read_body "$TMP/ctl_call" planted_init)"
case "$ctl" in
    "COUNTS 6 1 1 1 1 0") ;;
    *) fail "the reader answered [$ctl] for a planted body that calls out between the OR and the
  write. x0 is caller-saved and this reader carries its bit state straight through the call, so a
  body of this shape is decoded against registers the callee was free to destroy" ;;
esac

ctl_dead_reader "$(read_body "$TMP/ctl_lc" a_symbol_no_listing_carries)" \
    "a renamed per-core init would read as a clean one"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the bench PMU programming in $elf =="

rec="$(read_body "$TMP/dis" "$SYM")"
kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
total="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
writes="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
wrbits="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
wrdclear="$(printf '%s\n' "$rec" | cut -d' ' -f5)"
brmid="$(printf '%s\n' "$rec" | cut -d' ' -f6)"
brcond="$(printf '%s\n' "$rec" | cut -d' ' -f7)"
case "$kind" in
    NOSYM)
        fail "the disassembly of $elf carries no body for '$SYM', so the reader started
  nowhere. The per-core init was renamed, or the disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    NOOPEN)
        fail "the body of '$SYM' in $elf writes PMCR_EL0 nowhere across its $total
  instruction(s), so this gate has no write to read the value of. That is UNKNOWN and not a PMU
  left at its reset state: the symbol names a thunk over the real per-core init, the
  programming moved to a callee, or the register spelling in this disassembler moved. A
  two-instruction tail call carries one terminal branch, which the branch pre-check below
  excuses, so nothing else here would catch it" ;;
    COUNTS) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

require_number "$total" "the instruction count of $SYM"
require_number "$writes" "the PMCR_EL0 write count in $SYM"
require_number "$wrbits" "the count of PMCR_EL0 writes with E, C and LC bit-level set in $SYM"
require_number "$wrdclear" "the count of PMCR_EL0 writes with D bit-level clear in $SYM"
require_number "$brmid" "the count of branches before the last instruction of $SYM"
require_number "$brcond" "the count of conditional branches in $SYM"
echo "   corpus: $total instruction(s) in $SYM, $writes PMCR_EL0 write(s), $wrbits with E, C
   and LC bit-level set, $wrdclear with D bit-level clear, $brmid branch(es) before the last
   instruction and $brcond conditional branch(es)"

# BEFORE ANY VERDICT, because the verdicts below are read off ONE path.
if [ "$brmid" -ne 0 ] || [ "$brcond" -ne 0 ]; then
    fail "the body of '$SYM' in $elf carries $brmid branch(es) before its last instruction and
  $brcond conditional branch(es), and this reader models one path. A branch over the setup leaves
  a PMCR_EL0 write of whatever 'mrs' returned on the path not walked, and it is still exactly one
  write, so nothing below can see it. A call is the same hole: it returns to the next instruction
  and is free to destroy x0-x18, whose bit state this reader carries straight through. Only an
  unconditional branch as the LAST instruction is analysable, which is the tail call this body's
  own final call compiles to"
fi

rc=0

# Zero writes is refused above as UNKNOWN, so what reaches here is a body that programs the PMU
# more than once and leaves the last write deciding.
if [ "$writes" -gt 1 ]; then
    bad "the body of '$SYM' in $elf writes PMCR_EL0 $writes time(s) where it owes exactly one.
  The last write decides, and the bit arms below are stated over all of them, so a correct
  value followed by a wrong one satisfies neither this arm nor them by halves"
fi

if [ "$wrbits" -ne "$writes" ]; then
    bad "the body of '$SYM' in $elf writes PMCR_EL0 $writes time(s), of which only $wrbits
  carry E, C and LC each provably set in the value written. Tracking the destination register
  alone survives an instruction that clears one of those bits after the OR that set them; this
  reader tracks the bit itself, so that instruction breaks the chain. bench_cyccnt subtracts
  PMCCNTR_EL0 at 64 bits, and with LC clear that counter's overflow sits at bit 31, so a wrap
  enters the row as a near-2^32 sample or as a SAT. No run on any vehicle in this tree can tell
  the two images apart, which is why the bit is held here"
fi

if [ "$wrdclear" -ne "$writes" ]; then
    bad "the body of '$SYM' in $elf writes PMCR_EL0 $writes time(s), of which only $wrdclear
  carry D provably clear in the value written. D divides the cycle count by 64, and every
  figure the bench then reports still lands in a plausible range, so no vehicle in this tree
  tells that image apart from a correct one at run time either"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: '$SYM' programs PMCR_EL0 once, with E, C and LC each bit-level set and D
  bit-level clear in the value written, over a body this reader decoded end to end"
exit 0
