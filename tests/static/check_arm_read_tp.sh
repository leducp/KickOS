#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Structural gate on the emitted __aeabi_read_tp (arch/arm/read_tp.S).
#
# WHY A GATE AND NOT A RUNTIME WITNESS. The leaf answers every thread_local access on
# M-profile, and it derives the thread pointer from SP because unprivileged code has no
# register that differs per thread. The one way to get it wrong is at the EDGE: a stack top
# is exclusive, so an empty stack has SP exactly at base + stride and a plain mask returns
# the NEIGHBOUR's block. That happened on the first run of the tlsprobe witness and showed
# up as a data abort in the adjacent thread's memory; adding one unrelated local to main
# moved SP and hid it again. The witness therefore CANNOT be relied on to catch it: it
# passes with and without the subtract, because C code never runs with SP at the top.
#
# So this reads the instructions instead. Four of them, in order:
#   mov  r0, sp        take the only per-thread register unprivileged code has
#   subs r0, r0, #1    move SP into (base, base + stride] before masking
#   lsrs r0, r0, #N    mask down to the block. Shifts and not BIC: a Thumb-2 BIC
#   lsls r0, r0, #N    immediate is a modified immediate and 0x1FFF is not one.
#   bx   lr
#
# Also asserts nothing else appears: a memory access here would fault on an enforcing
# board, and any register above r0 would break the AEABI restricted-clobber rule that lets
# the compiler keep r1-r3 and the whole VFP live across the call.
#
# usage: check_arm_read_tp.sh <objdump> <arch.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

OBJDUMP="${1:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"
ARCHIVE="${2:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"

command -v "$OBJDUMP" >/dev/null 2>&1 || fail "objdump not found: $OBJDUMP"
[ -f "$ARCHIVE" ] || fail "no archive at $ARCHIVE"

TMP="$(mktemp)" || fail "mktemp failed"
trap 'rm -f "$TMP"' EXIT

"$OBJDUMP" -d --section=.text.__aeabi_read_tp "$ARCHIVE" > "$TMP" 2>/dev/null \
    || fail "objdump failed on $ARCHIVE"

grep -q '<__aeabi_read_tp>:' "$TMP" \
    || fail "no __aeabi_read_tp in $ARCHIVE. Every thread_local access on M-profile calls
    it and neither libc.a nor libgcc.a defines it, so its absence is a link error waiting
    for the first app that declares one."

# The mnemonic column only, in order, from the function body.
# objdump separates offset, encoding, mnemonic and operands with TABS, so split on those
# rather than on runs of spaces: the encoding column is space-padded and a space-based
# split reads it as part of the mnemonic.
BODY="$(sed -n '/<__aeabi_read_tp>:/,/^$/p' "$TMP" \
        | awk -F'\t' '$3 != "" { print $3 " " $4 }' \
        | tr -s ' ' | sed 's/ *$//')"
[ -n "$BODY" ] || fail "could not read the disassembled body of __aeabi_read_tp"

EXPECT_N=5
GOT_N="$(printf '%s\n' "$BODY" | wc -l)"
[ "$GOT_N" -eq "$EXPECT_N" ] || {
    printf '%s\n' "$BODY"
    fail "__aeabi_read_tp is $GOT_N instructions, expected $EXPECT_N"
}

printf '%s\n' "$BODY" | sed -n 1p | grep -q '^mov r0, sp$' \
    || fail "__aeabi_read_tp does not start by taking SP"
printf '%s\n' "$BODY" | sed -n 2p | grep -q '^subs r0, #1$\|^subs r0, r0, #1$' \
    || {
        printf '%s\n' "$BODY"
        fail "__aeabi_read_tp does not subtract one before masking. A stack top is
    EXCLUSIVE, so an empty stack has SP exactly at base + stride and the mask below would
    return the NEIGHBOUR's block. The tlsprobe witness does NOT catch this: C code never
    runs with SP at the top, so it passes either way."
    }
printf '%s\n' "$BODY" | sed -n 3p | grep -q '^lsrs r0, r0, #[0-9]*$' \
    || fail "__aeabi_read_tp instruction 3 is not the lsrs half of the mask"
printf '%s\n' "$BODY" | sed -n 4p | grep -q '^lsls r0, r0, #[0-9]*$' \
    || fail "__aeabi_read_tp instruction 4 is not the lsls half of the mask"
printf '%s\n' "$BODY" | sed -n 5p | grep -q '^bx lr$' \
    || fail "__aeabi_read_tp does not end in bx lr"

SHR="$(printf '%s\n' "$BODY" | sed -n 3p | sed 's/.*#//')"
SHL="$(printf '%s\n' "$BODY" | sed -n 4p | sed 's/.*#//')"
[ "$SHR" = "$SHL" ] || fail "__aeabi_read_tp shifts right by $SHR and left by $SHL"

echo "PASS: __aeabi_read_tp masks SP-1 down to a 1 << $SHR stride in five instructions"
