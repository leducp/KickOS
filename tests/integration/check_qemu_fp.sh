#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate for the FP context-switch test: boot `fp_switch` on a QEMU Cortex-M4F, or read a silicon
# capture of it, and assert the callee-saved FP bank (s16-s31) survived the checker's rounds
# against the trasher. Proves the PendSV FP save/restore on armv7m.
#
#   check_qemu_fp.sh <fp_switch.elf>
#   KOS_CAPTURE=<log> check_qemu_fp.sh <board-build> <kickos-source> <cmake>
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

# The colon is what matches a result rather than the banner text.
if judging_capture; then
    judge_capture fp_switch
    POLL_OK=0
    if has "FP OK:"; then
        POLL_OK=1
    fi
else
    elf="${1:?usage: check_qemu_fp.sh <fp_switch.elf>}"
    poll_image "$elf" "FP OK:"
fi

# Reported before the bound is, so a corruption on the wire is named as one instead of as an
# app that produced nothing.
if has "FP FAIL:"; then
    printf '%s\n' "$OUT" | grep "FP FAIL:"
    cfail error "FP register bank corrupted across a context switch"
fi
if [ "$POLL_OK" -ne 1 ]; then
    cfail verdict "no 'FP OK:' line: the app completed no batch of rounds with s16-s31 intact
  (it did not boot, or every round it judged was corrupt)"
fi

assert_no_panic "fp_switch panicked"

echo "PASS: s16-s31 preserved across context switches"
exit 0
