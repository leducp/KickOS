#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU armv7m gate for the FP context-switch test: boot `fp_switch` on a QEMU
# Cortex-M4F, let the checker run a few rounds against the trasher, then assert
# the callee-saved FP bank (s16-s31) survives context switches -- "FP OK" must
# appear and "FP FAIL" must not. Proves the PendSV FP save/restore on real armv7m.

set -u
elf="${1:?usage: check_qemu_fp.sh <fp_switch.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "SKIP: $qemu not found"
    exit 77
fi

out="$(timeout 6 "$qemu" -M "$machine" -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

# Match the result lines (with a colon), not the app's descriptive banner text.
if echo "$out" | grep -q "FP FAIL:"; then
    echo "FAIL: FP register bank corrupted across a context switch"
    exit 1
fi
if echo "$out" | grep -q "FP OK:"; then
    echo "PASS: s16-s31 preserved across context switches"
    exit 0
fi
echo "FAIL: no FP result seen (did the app boot / run?)"
exit 1
