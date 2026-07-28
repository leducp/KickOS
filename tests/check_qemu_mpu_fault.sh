#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU MPU-enforcement gate: boot the `mpu_fault` image on QEMU (semihosting
# console) and assert the deliberate cross-domain write TRAPS. An unprivileged
# domain-A thread writes its own granted region (must succeed), then writes domain
# B's region (must fault). The "cross-domain write completed" line (the
# no-enforcement path) must NOT appear. mpu_fault self-terminates, so QEMU_TIMEOUT is
# only a hang backstop.
#
# Registered only on a build configured with -DKICKOS_HAVE_MPU=1; on a
# no-enforcement build the write completes, which is correct there and would
# (rightly) fail this gate.

set -u
elf="${1:?usage: check_qemu_mpu_fault.sh <mpu_fault.elf>}"
# ARM defaults, RISC-V by environment -- the same split every other gate script uses
# (kickos_add_qemu_test supplies QEMU/QEMU_MACHINE/QEMU_EXTRA per board). This script
# was RISC-V-defaulted while PMP was the only enforcing target QEMU could run; the
# defaults moved when the mps2 boards joined, so that a missing env is a wrong-machine
# error on ARM rather than a silent attempt to boot an ARM ELF on qemu-system-riscv32.
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

if echo "$out" | grep -qE "ERROR|did not fault|cross-domain write completed"; then
    echo "FAIL: cross-domain write was NOT trapped (enforcement inactive?)"
    exit 1
fi
# The trap is proven by one of two markers, because the two families report from
# different places. RISC-V takes the KERNEL-reported path: the trap is delivered to the
# kernel's handler, which names the offending task.
if echo "$out" | grep -q "MPU FAULT: task 'domainA'"; then
    echo "PASS: unprivileged cross-domain write took a kernel-reported MPU trap"
    exit 0
fi
# ARM never reaches that path: the MemManage exception is taken straight to
# kickos_armv7m_fault_report, which dumps the frame and terminates -- so no task name
# is ever printed and only its banner is available. That banner is still a STRICT
# marker: the reporter labels the dump "MPU FAULT" only when the CFSR MMFSR byte is
# set (arch/arm/armv7m/arch_armv7m.cc), so any other fault reads "HARD FAULT" and does
# NOT satisfy this gate.
if echo "$out" | grep -q "=== MPU FAULT ==="; then
    echo "PASS: unprivileged cross-domain write took a MemManage trap (CFSR MMFSR set)"
    exit 0
fi
echo "FAIL: MPU FAULT marker missing (crash / hang / truncated run?)"
exit 1
