#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU kos_panic wire gate: boot a `panicgate` image and assert the expected panic line
# reached the wire. The positive peer of check_qemu_rootauth.sh's absence assertion,
# which passes trivially when the panic path is never taken.
#
# $2 is the literal line expected after the kernel's trusted banner; $3, when non-empty,
# is a literal that must NOT appear (the tail a truncation drops). A FAULT banner is a
# failure even when the expected line is also present: the panic must come from the
# syscall, not from the kernel dereferencing the caller's pointer.

set -u
elf="${1:?usage: check_qemu_panicgate.sh <panicgate.elf> <expected-line> [absent]}"
expect="${2:?usage: check_qemu_panicgate.sh <panicgate.elf> <expected-line> [absent]}"
absent="${3:-}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-20}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

if echo "$out" | grep -q "\[panicgate\] ERROR"; then
    echo "FAIL: kos_panic returned to the caller"
    exit 1
fi
# The arm must have been reached, so a panic cannot be credited to an earlier trap
# during ctors or bring-up.
if ! echo "$out" | grep -q "\[panicgate\] case"; then
    echo "FAIL: the app never reached its kos_panic call"
    exit 1
fi
if echo "$out" | grep -qE "=== (HARD|MPU|BUS) FAULT|=== RISC-V TRAP|MPU FAULT: task"; then
    echo "FAIL: the kernel faulted instead of refusing the message pointer"
    exit 1
fi
if ! echo "$out" | grep -qF -- "$expect"; then
    echo "FAIL: expected panic line missing: $expect"
    exit 1
fi
if [ -n "$absent" ] && echo "$out" | grep -qF -- "$absent"; then
    echo "FAIL: text that must not reach the wire is present: $absent"
    exit 1
fi
echo "PASS: $expect"
exit 0
