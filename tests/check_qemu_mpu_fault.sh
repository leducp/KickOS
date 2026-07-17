#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU MPU-enforcement gate: boot the `mpu_fault` image on QEMU (semihosting
# console) and assert the deliberate cross-domain write TRAPS. An unprivileged
# domain-A thread writes its own granted region (must succeed), then writes domain
# B's region (must fault). Under real enforcement the kernel reports
# "MPU FAULT: task 'domainA'" and shuts down; the "cross-domain write completed"
# line (the no-enforcement path) must NOT appear. mpu_fault self-terminates, so
# QEMU_TIMEOUT is only a hang backstop.
#
# Registered only for a qemu-riscv build configured with -DKICKOS_HAVE_MPU=1 (the
# RISC-V PMP enforcement variant); on a no-enforcement build the write completes,
# which is correct there and would (rightly) fail this gate.

set -u
elf="${1:?usage: check_qemu_mpu_fault.sh <mpu_fault.elf>}"
qemu="${QEMU:-qemu-system-riscv32}"
machine="${QEMU_MACHINE:-virt}"
extra_arg="${QEMU_EXTRA:--bios none}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-20}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

if echo "$out" | grep -qE "ERROR|did not fault|cross-domain write completed"; then
    echo "FAIL: cross-domain write was NOT trapped (enforcement inactive?)"
    exit 1
fi
if echo "$out" | grep -q "MPU FAULT: task 'domainA'"; then
    echo "PASS: unprivileged cross-domain write took a PMP trap"
    exit 0
fi
echo "FAIL: MPU FAULT marker missing (crash / hang / truncated run?)"
exit 1
