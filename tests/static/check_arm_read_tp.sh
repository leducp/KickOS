#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Structural gate on the emitted __aeabi_read_tp (arch/arm/read_tp.S).
#
# The one way to get this leaf wrong is at the EDGE: a stack top is exclusive, so an empty
# stack has SP exactly at base + stride and a plain mask returns the NEIGHBOUR's block. The
# tlsprobe witness passes with and without the subtract, C code never running with SP at the
# top, so the INSTRUCTIONS are what gets read. Five of them, in order:
#   mov  r0, sp
#   subs r0, r0, #1    move SP into (base, base + stride] before masking
#   lsrs r0, r0, #N    shifts and not BIC: a Thumb-2 BIC immediate is a modified immediate
#   lsls r0, r0, #N    and 0x1FFF is not one
#   bx   lr
#
# Nothing else may appear: a memory access here would fault on an enforcing board, and any
# register above r0 breaks the AEABI restricted-clobber rule that lets the compiler keep r1-r3
# and the whole VFP live across the call.
#
# usage: check_arm_read_tp.sh <objdump> <arch.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

OBJDUMP="${1:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"
ARCHIVE="${2:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"

command -v "$OBJDUMP" >/dev/null 2>&1 || fail "objdump not found: $OBJDUMP"
[ -f "$ARCHIVE" ] || fail "no archive at $ARCHIVE"

scratch_dir

# The two spellings the assembler may pick for the subtract, and the shift amount, handed to
# the judge rather than written inside it, so the controls below and the real read cannot
# disagree about what the rule is.
SUBS_ERE='^subs r0, #1$|^subs r0, r0, #1$'
SHIFT_DIGITS='[0-9][0-9]*'

judge() { # <disassembly> <subs-ere> <shift-digits-ere>
    _dis="$1"
    _subs="$2"
    _sh="$3"

    grep -q '<__aeabi_read_tp>:' "$_dis" \
        || fail "no __aeabi_read_tp in $ARCHIVE. Every thread_local access on M-profile calls
    it and neither libc.a nor libgcc.a defines it, so its absence is a link error waiting
    for the first app that declares one."

    # The mnemonic column only, in order. objdump separates its columns with TABS and the
    # encoding column is space-padded, so a space-based split would read it as part of the
    # mnemonic.
    _body="$(sed -n '/<__aeabi_read_tp>:/,/^$/p' "$_dis" \
            | awk -F'\t' '$3 != "" { print $3 " " $4 }' \
            | tr -s ' ' | sed 's/ *$//')"
    [ -n "$_body" ] || fail "could not read the disassembled body of __aeabi_read_tp"

    _n="$(printf '%s\n' "$_body" | wc -l | tr -d ' ')"
    [ "$_n" -eq 5 ] || {
        printf '%s\n' "$_body"
        fail "__aeabi_read_tp is $_n instructions, expected 5"
    }

    printf '%s\n' "$_body" | sed -n 1p | grep -q '^mov r0, sp$' \
        || fail "__aeabi_read_tp does not start by taking SP"
    printf '%s\n' "$_body" | sed -n 2p | grep -qE "$_subs" \
        || {
            printf '%s\n' "$_body"
            fail "__aeabi_read_tp does not subtract one before masking. A stack top is
    EXCLUSIVE, so an empty stack has SP exactly at base + stride and the mask below would
    return the NEIGHBOUR's block. The tlsprobe witness does NOT catch this: C code never
    runs with SP at the top, so it passes either way."
        }
    printf '%s\n' "$_body" | sed -n 3p | grep -qE "^lsrs r0, r0, #$_sh\$" \
        || fail "__aeabi_read_tp instruction 3 is not the lsrs half of the mask"
    printf '%s\n' "$_body" | sed -n 4p | grep -qE "^lsls r0, r0, #$_sh\$" \
        || fail "__aeabi_read_tp instruction 4 is not the lsls half of the mask"
    printf '%s\n' "$_body" | sed -n 5p | grep -q '^bx lr$' \
        || fail "__aeabi_read_tp does not end in bx lr"

    _shr="$(printf '%s\n' "$_body" | sed -n 3p | sed 's/.*#//')"
    _shl="$(printf '%s\n' "$_body" | sed -n 4p | sed 's/.*#//')"
    [ "$_shr" = "$_shl" ] || fail "__aeabi_read_tp shifts right by $_shr and left by $_shl"

    echo "PASS: __aeabi_read_tp masks SP-1 down to a 1 << $_shr stride in five instructions"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# THE CONTROLS ARE FORGED objdump OUTPUT, so what they prove is the JUDGE, never the
# invocation: that the disassembly comes from .text.__aeabi_read_tp of the archive named on
# the command line is outside them.
#
# objdump separates the address, encoding and mnemonic columns with TABS and the judge splits
# on them, so a forged line built with spaces would be read differently from a real one.
plant() { # <outfile> <symbol> <mnemonic;operands>...
    _f="$1"
    _s="$2"
    shift 2
    printf '00000000 <%s>:\n' "$_s" > "$_f"
    _off=0
    for _i in "$@"; do
        printf '   %d:\t4668      \t%s\t%s\n' "$_off" "${_i%%;*}" "${_i#*;}" >> "$_f"
        _off=$((_off + 2))
    done
    printf '\n' >> "$_f"
}

I_SP='mov;r0, sp'
I_SUB2='subs;r0, #1'
I_SUB3='subs;r0, r0, #1'
I_LSR='lsrs;r0, r0, #10'
I_LSL='lsls;r0, r0, #10'
I_RET='bx;lr'

# Each positive differs from the clean encoding in ONE property, so no other clause can be
# what reddens it. The count of instructions is held at five wherever the clause under test
# is not the count itself.
plant "$TMP/p_nosym"  __aeabi_get_tp  "$I_SP" "$I_SUB2" "$I_LSR" "$I_LSL" "$I_RET"
plant "$TMP/p_extra"  __aeabi_read_tp "$I_SP" "$I_SUB2" "$I_LSR" "$I_LSL" "$I_RET" 'nop;'
plant "$TMP/p_notsp"  __aeabi_read_tp 'mov;r0, r1' "$I_SUB2" "$I_LSR" "$I_LSL" "$I_RET"
plant "$TMP/p_noadj"  __aeabi_read_tp "$I_SP" 'adds;r0, r0, #1' "$I_LSR" "$I_LSL" "$I_RET"
plant "$TMP/p_nolsr"  __aeabi_read_tp "$I_SP" "$I_SUB2" 'bic;r0, r0, #8191' "$I_LSL" "$I_RET"
plant "$TMP/p_nolsl"  __aeabi_read_tp "$I_SP" "$I_SUB2" "$I_LSR" 'nop;' "$I_RET"
plant "$TMP/p_noret"  __aeabi_read_tp "$I_SP" "$I_SUB2" "$I_LSR" "$I_LSL" 'bx;r3'
plant "$TMP/p_widths" __aeabi_read_tp "$I_SP" "$I_SUB2" "$I_LSR" 'lsls;r0, r0, #11' "$I_RET"
# A symbol objdump printed with no instruction under it: the body reader must refuse rather
# than judge one instruction it never read.
printf '00000000 <__aeabi_read_tp>:\n\n' > "$TMP/p_nobody"

# The negatives differ in a property the rule TOLERATES, and every one is read on its own so
# a control silent for the wrong reason is visible. n_framed carries the preamble and the
# following symbol a real run has, which is what proves the body reader bounds its range.
plant "$TMP/n_subs2"  __aeabi_read_tp "$I_SP" "$I_SUB2" "$I_LSR" "$I_LSL" "$I_RET"
plant "$TMP/n_subs3"  __aeabi_read_tp "$I_SP" "$I_SUB3" "$I_LSR" "$I_LSL" "$I_RET"
plant "$TMP/n_stride" __aeabi_read_tp "$I_SP" "$I_SUB3" 'lsrs;r0, r0, #13' 'lsls;r0, r0, #13' "$I_RET"
{
    printf '%s\n' 'libkickos_arch_armv7m.a(read_tp.S.obj):     file format elf32-littlearm'
    printf '\n\nDisassembly of section .text.__aeabi_read_tp:\n\n'
    cat "$TMP/n_subs3"
    printf '00000010 <kos_neighbour>:\n   10:\t4770      \tbx\tlr\n\n'
} > "$TMP/n_framed"

NEGATIVES='n_subs2 n_subs3 n_stride n_framed'

FIRED=0
ctl() { # <label> <expect-ere> <disassembly>
    if ( judge "$3" "$SUBS_ERE" "$SHIFT_DIGITS" ) > "$TMP/out" 2>&1; then
        cat "$TMP/out"
        fail "positive control '$1' passed, so the clause it plants for fires on nothing"
    fi
    grep -qE "$2" "$TMP/out" || {
        cat "$TMP/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/. A control
    another clause catches proves nothing about the clause it plants for"
    }
    FIRED=$((FIRED + 1))
}

ctl nosym  'no __aeabi_read_tp in '                    "$TMP/p_nosym"
ctl nobody 'could not read the disassembled body'      "$TMP/p_nobody"
ctl extra  'is 6 instructions, expected 5'             "$TMP/p_extra"
ctl notsp  'does not start by taking SP'               "$TMP/p_notsp"
ctl noadj  'does not subtract one before masking'      "$TMP/p_noadj"
ctl nolsr  'instruction 3 is not the lsrs half'        "$TMP/p_nolsr"
ctl nolsl  'instruction 4 is not the lsls half'        "$TMP/p_nolsl"
ctl noret  'does not end in bx lr'                     "$TMP/p_noret"
ctl widths 'shifts right by 10 and left by 11'         "$TMP/p_widths"
[ "$FIRED" -eq 9 ] || fail "$FIRED of 9 positive controls reddened"

QUIET=0
for C in $NEGATIVES; do
    if ( judge "$TMP/$C" "$SUBS_ERE" "$SHIFT_DIGITS" ) > "$TMP/out" 2>&1; then
        QUIET=$((QUIET + 1))
    else
        cat "$TMP/out" >&2
        fail "negative control '$C' reports, so the gate would redden a legal encoding"
    fi
done
[ "$QUIET" -eq 4 ] || fail "$QUIET of 4 negative controls ran silent"

# Withdraw one tolerance and the number of negatives that report must MOVE by an EXACT
# amount: a negative kept quiet by the wrong clause then shows up as the wrong number.
mutate() { # <what> <subs-ere> <shift-digits-ere> <expect-count>
    _m=0
    for _c in $NEGATIVES; do
        if ( judge "$TMP/$_c" "$2" "$3" ) > /dev/null 2>&1; then
            :
        else
            _m=$((_m + 1))
        fi
    done
    [ "$_m" -eq "$4" ] || fail "with the $1 withdrawn, $_m negative control(s) report, expected $4;
    the controls for it are not near misses and prove nothing"
}
mutate "two-operand subtract" '^subs r0, r0, #1$' "$SHIFT_DIGITS" 1
mutate "free shift amount"    "$SUBS_ERE"         '10'            1

# --- the archive ---------------------------------------------------------------
tool_out "$TMP/dis" '' "$OBJDUMP" -d --section=.text.__aeabi_read_tp "$ARCHIVE"
judge "$TMP/dis" "$SUBS_ERE" "$SHIFT_DIGITS"
