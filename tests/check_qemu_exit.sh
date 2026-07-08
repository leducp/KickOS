#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU regression gate: a non-last thread that exits must not panic. Boot
# sched_exit on QEMU; assert the worker exited AND root ran past it (the survival
# marker), and that no panic/unreachable fired on the deferred switch-away.

set -u
elf="${1:?usage: check_qemu_exit.sh <sched_exit.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"   # armv7m default; microbit for armv6m
extra_arg="${QEMU_EXTRA:-}"             # e.g. -icount shift=auto (CI: coarse mps2 timer)

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

out="$(timeout "${QEMU_TIMEOUT:-8}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

if echo "$out" | grep -qiE "panic|unreachable"; then
    echo "FAIL: panic/unreachable on non-last thread exit"
    exit 1
fi
if echo "$out" | grep -q "worker: exiting" \
   && echo "$out" | grep -q "root: survived worker exit"; then
    echo "PASS: non-last thread exit did not panic"
    exit 0
fi
echo "FAIL: expected markers missing (worker did not exit or root did not survive)"
exit 1
