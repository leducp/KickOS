#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU armv7m gate for the FP context-switch test: boot `fp_switch` on a QEMU
# Cortex-M4F, let the checker run a few rounds against the trasher, then assert
# the callee-saved FP bank (s16-s31) survives context switches: "FP OK" must
# appear and "FP FAIL" must not. Proves the PendSV FP save/restore on real armv7m.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=6}"

elf="${1:?usage: check_qemu_fp.sh <fp_switch.elf>}"

# The checker loops forever, so it is polled until a RESULT line lands ("FP FAIL:" ->
# corruption, "FP OK:" -> a clean 10-round batch) rather than burning the whole timeout.
# The colon is what matches a result and not the banner text.
poll_image "$elf" "FP (OK|FAIL):"

if [ "$POLL_OK" -ne 1 ]; then
    fail "no FP result seen (did the app boot / run?)"
fi
if has "FP FAIL:"; then
    fail "FP register bank corrupted across a context switch"
fi

echo "PASS: s16-s31 preserved across context switches"
exit 0
