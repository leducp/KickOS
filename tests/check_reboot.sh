#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Plumbing gate for kos_reboot: boot rebootdemo on a declining-fallback target (no QEMU
# machine models a bootrom download mode), so the whole path (root's kos_reboot,
# the AUTH_SYSTEM gate, the synchronous console flush, the arch_reboot fallback) must
# come back with -KOS_ENOSYS and the app must then shut down cleanly. arch_shutdown
# forwards the status over semihosting, so QEMU's exit code IS the clean-exit half.
# Native run for the sim; QEMU (semihosting) when QEMU_MACHINE is set.
#
# rc=-38 is -KOS_ENOSYS (system/include/kickos/sys/errno.h).

set -u
elf="${1:?usage: check_reboot.sh <rebootdemo.elf>}"

if [ -n "${QEMU_MACHINE:-}" ]; then
    qemu="${QEMU:-qemu-system-arm}"
    extra_arg="${QEMU_EXTRA:-}" # e.g. -bios none (RISC-V virt)
    if ! command -v "$qemu" >/dev/null 2>&1; then
        # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
        echo "SKIP: $qemu not found"
        exit 77
    fi
    # shellcheck disable=SC2086
    out="$(timeout "${QEMU_TIMEOUT:-20}" "$qemu" -M "$QEMU_MACHINE" $extra_arg \
             -nographic -semihosting -kernel "$elf" 2>&1)"
    rc=$?
else
    out="$(timeout "${QEMU_TIMEOUT:-20}" "$elf" 2>&1)"
    rc=$?
fi
echo "$out"

# The reporters' literal dump markers (kpanic, the armv7m/armv6m/sim/riscv fault
# reporters, kickos_isr_fault). Case-sensitive and anchored on the banner shape: a
# substring match on "fault" also hits "EFAULT" and "default" in benign output.
if echo "$out" | grep -qE "KERNEL PANIC:|=== (HARD|MPU|SIM) FAULT|=== RISC-V TRAP|MPU FAULT: task|ISOLATION FAULT:"; then
    echo "FAIL: panic/fault during rebootdemo"
    exit 1
fi
if [ "$rc" -eq 124 ]; then
    echo "FAIL: rebootdemo timed out (no exit status forwarded)"
    exit 1
fi
if [ "$rc" -ne 0 ]; then
    echo "FAIL: rebootdemo exit status $rc (expected a clean shutdown)"
    exit 1
fi
if ! echo "$out" | grep -q "reboot declined: rc=-38"; then
    echo "FAIL: the -KOS_ENOSYS refusal line is missing"
    exit 1
fi
echo "PASS: kos_reboot declined with -KOS_ENOSYS and the app shut down cleanly"
exit 0
