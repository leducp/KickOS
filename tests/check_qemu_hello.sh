#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU smoke gate for the armv7m target: boot the `hello` image on a QEMU
# Cortex-M4 (mps2-an386) via semihosting, let the two userspace threads
# ping-pong for a few seconds, then assert the exchange happened. This exercises
# the whole M1 arch layer end to end on real ARM: reset -> scheduler start
# (PendSV) -> SVC-trampoline syscalls -> SysTick timer -> semaphore reschedule.

set -u
elf="${1:?usage: check_qemu_hello.sh <hello.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"   # armv7m default; microbit for armv6m
cpu_arg=""
if [ -n "${QEMU_CPU:-}" ]; then
    cpu_arg="-cpu ${QEMU_CPU}"
fi

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 (not 0): CTest treats this as SKIP, not PASS, so a CI box without
    # QEMU does not silently green-light the gate.
    echo "SKIP: $qemu not found"
    exit 77
fi

out="$(timeout 8 "$qemu" -M "$machine" $cpu_arg -nographic -semihosting \
        -kernel "$elf" 2>&1)"

echo "$out"

# Require the banner (boot + console) and several ping-pong rounds (scheduling +
# syscalls + timer all live).
if echo "$out" | grep -q "KickOS" \
   && echo "$out" | grep -q "ping 3" \
   && echo "$out" | grep -q "pong 3"; then
    echo "PASS: QEMU armv7m hello ping-ponged"
    exit 0
fi

echo "FAIL: expected banner + ping/pong rounds not observed"
exit 1
