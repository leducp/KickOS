#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU regression gate (M4.3): a root pre-publish printf must not poison a
# post-publish worker's stdout route. Boot initdemo on QEMU (mps2-an386) via
# semihosting. The console is DARK after publish, so the verdict rides the EXIT
# STATUS: initdemo returns 0 iff the software console driver received exactly the
# worker's payload byte count, else 1. arch_shutdown forwards that via semihosting
# SYS_EXIT_EXTENDED, so QEMU's process exit code IS the verdict.
#
# PRE-fix (sticky process-wide probe): worker bytes bypass the endpoint -> count 0
# -> exit 1 -> FAIL. POST-fix (per-invocation re-probe): exit 0 -> PASS.

set -u
elf="${1:?usage: check_qemu_initdemo.sh <initdemo.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-15}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
rc=$?
echo "$out"

if echo "$out" | grep -qiE "panic|unreachable"; then
    echo "FAIL: panic/unreachable during initdemo"
    exit 1
fi
if [ "$rc" -eq 124 ]; then
    echo "FAIL: initdemo timed out (no exit status forwarded)"
    exit 1
fi
if [ "$rc" -eq 0 ]; then
    echo "PASS: post-publish worker's stdout reached the endpoint (per-thread route)"
    exit 0
fi
echo "FAIL: initdemo exit status $rc (worker bytes did not reach the endpoint)"
exit 1
