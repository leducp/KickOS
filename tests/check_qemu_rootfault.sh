#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU ROOT-confinement gate: boot the `rootfault` image and assert that ROOT's write
# into a child's granted region TRAPS. The peer of check_qemu_mpu_fault.sh, and NOT the
# same claim: that one proves a spawned child is confined, this one proves the thread
# that ran the ctors and the board bring-up is.
#
# Registered only on a build configured -DKICKOS_HAVE_MPU=1, so the app's own
# "NOT confined" line is a failure marker here, exactly as mpu_fault treats its
# no-enforcement line. rootfault self-terminates either way, so QEMU_TIMEOUT is only a
# hang backstop.

set -u
elf="${1:?usage: check_qemu_rootfault.sh <rootfault.elf>}"
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

if echo "$out" | grep -qE "ERROR|NOT confined"; then
    echo "FAIL: root's cross-domain write was NOT trapped (enforcement off?)"
    exit 1
fi
# The CONTROL half must have run: the child writing its own granted region proves the
# grant machinery worked and that the fault below is about root's confinement, not about
# a region that was never mapped in the first place.
if ! echo "$out" | grep -q "child: wrote my own granted region"; then
    echo "FAIL: the child never wrote its granted region (setup failed before the test)"
    exit 1
fi
# ...and root must have reached the poke, so a fault cannot be credited to an earlier
# unrelated trap during ctors or bring-up.
if ! echo "$out" | grep -q "root: writing the child's granted region"; then
    echo "FAIL: root never reached the deliberate write (faulted earlier?)"
    exit 1
fi
# Same two-family split as check_qemu_mpu_fault.sh: RISC-V traps to the kernel handler,
# which names the task; ARM takes MemManage straight to the armv7m reporter, which prints
# no name but labels the dump "MPU FAULT" only when the CFSR MMFSR byte is set.
if echo "$out" | grep -q "MPU FAULT: task 'root'"; then
    echo "PASS: root took a kernel-reported MPU trap on the cross-domain write"
    exit 0
fi
if echo "$out" | grep -q "=== MPU FAULT ==="; then
    echo "PASS: root took a MemManage trap on the cross-domain write (CFSR MMFSR set)"
    exit 0
fi
echo "FAIL: MPU FAULT marker missing (crash / hang / truncated run?)"
exit 1
