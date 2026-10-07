#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Structural gate on the emitted __aeabi_read_tp (arch/arm/read_tp.S): its disassembly, mnemonic
# and operands, is exactly the listing below, the shift read off its own lsrs.
#
# The one way to get this leaf wrong is at the EDGE: a stack top is exclusive, so an empty
# stack has SP exactly at base + stride and a plain mask returns the NEIGHBOUR's block. The
# tlsprobe witness passes with and without the subtract, C code never running with SP at the
# top, so the INSTRUCTIONS are what gets read. Nothing else may appear: a memory access here
# would fault on an enforcing board, and any register above r0 breaks the AEABI
# restricted-clobber rule that lets the compiler keep r1-r3 and the whole VFP live across the
# call.
#
# usage: check_arm_read_tp.sh <objdump> <arch.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

OBJDUMP="${1:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"
ARCHIVE="${2:?usage: check_arm_read_tp.sh <objdump> <arch.a>}"
[ -f "$ARCHIVE" ] || fail "no archive at $ARCHIVE"

scratch_dir

# <listing> -> the body's mnemonic and operands, one per line, against the expected listing.
judge() { # <disassembly>
    sed -n '/<__aeabi_read_tp>:/,/^$/p' "$1" | awk -F'\t' '$3 != "" { print $3 " " $4 }' \
        | tr -s ' ' | sed 's/ *$//' > "$TMP/body"
    require_nonempty "$TMP/body" "no __aeabi_read_tp body in $ARCHIVE. Every thread_local access
      on M-profile calls it and neither libc.a nor libgcc.a defines it"
    _n="$(sed -n 's/^lsrs r0, r0, #\([0-9][0-9]*\)$/\1/p' "$TMP/body")"
    printf '%s\n' 'mov r0, sp' 'subs r0, #1' "lsrs r0, r0, #$_n" "lsls r0, r0, #$_n" 'bx lr' \
        > "$TMP/want"
    diff "$TMP/want" "$TMP/body" >&2 || fail "__aeabi_read_tp is not mov, subs #1, the two shifts
      and bx lr. Without the subtract an empty stack's SP, at its exclusive top, masks to the
      NEIGHBOUR's block"
}

tool_out "$TMP/dis" '<__aeabi_read_tp>:' "$OBJDUMP" -d --section=.text.__aeabi_read_tp "$ARCHIVE"
judge "$TMP/dis"
SHIFT="$_n"
# The planted defect: the same listing without its subtract.
grep -v "${TAB}subs${TAB}" "$TMP/dis" > "$TMP/planted"
if ( judge "$TMP/planted" ) > /dev/null 2>&1; then
    fail "the listing with its subs removed still matched, so the diff judges nothing"
fi
echo "PASS: __aeabi_read_tp masks SP-1 down to a 1 << $SHIFT stride in five instructions, and
      the same listing without its subtract is refused"
