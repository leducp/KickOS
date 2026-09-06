#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU armv7m gate for the FP context-switch test: boot `fp_switch` on a QEMU
# Cortex-M4F, let the checker run a few rounds against the trasher, then assert
# the callee-saved FP bank (s16-s31) survives context switches. Proves the PendSV
# FP save/restore on real armv7m.
#
# THE VERDICT IS THE OK LINE, NOT THE ABSENCE OF THE FAIL ONE. The app prints a result every
# round it judges and only advances its counter on a clean one, so a bank that stays corrupted
# never reaches a tenth good round and no OK line is ever printed: waiting for one is a positive
# claim, where a grep for "FP FAIL:" on a four-core wire can only be wrong towards a pass.
#
# The FAIL grep is kept for a corruption that RECOVERED, which the OK line alone cannot see.
# That one is weak by nature: a shuffle hides the line it reads.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=6}"

elf="${1:?usage: check_qemu_fp.sh <fp_switch.elf>}"

# The colon is what matches a result rather than the banner text.
poll_image "$elf" "FP OK:"

# Reported before the bound is, so a corruption on the wire is named as one instead of as an
# app that produced nothing.
if has "FP FAIL:"; then
    printf '%s\n' "$OUT" | grep "FP FAIL:"
    fail "FP register bank corrupted across a context switch"
fi
if [ "$POLL_OK" -ne 1 ]; then
    fail "no clean FP batch in ${QEMU_TIMEOUT}s: the app never reached ten consecutive rounds
  with s16-s31 intact (it did not boot, or every round it judged was corrupt)"
fi

echo "PASS: s16-s31 preserved across context switches"
exit 0
