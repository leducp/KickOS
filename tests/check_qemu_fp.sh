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
extra_arg="${QEMU_EXTRA:-}"             # e.g. -icount shift=auto (CI: coarse mps2 timer)

if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "SKIP: $qemu not found"
    exit 77
fi

# The checker loops forever; poll and stop QEMU as soon as a result line appears
# ("FP FAIL:" -> corruption; "FP OK:" -> a clean 10-round batch), rather than
# burning the whole timeout. Match the result lines (colon), not the banner text.
log="$(mktemp)"
# shellcheck disable=SC2086
"$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" >"$log" 2>&1 &
qpid=$!
verdict=""
for _ in $(seq 1 $(( ${QEMU_TIMEOUT:-6} * 5 ))); do   # poll at 5 Hz
    if grep -q "FP FAIL:" "$log"; then verdict="fail"; break; fi
    if grep -q "FP OK:" "$log"; then verdict="ok"; break; fi
    kill -0 "$qpid" 2>/dev/null || break
    sleep 0.2
done
{ kill "$qpid"; wait "$qpid"; } 2>/dev/null
cat "$log"; rm -f "$log"

if [ "$verdict" = "fail" ]; then
    echo "FAIL: FP register bank corrupted across a context switch"
    exit 1
fi
if [ "$verdict" = "ok" ]; then
    echo "PASS: s16-s31 preserved across context switches"
    exit 0
fi
echo "FAIL: no FP result seen (did the app boot / run?)"
exit 1
