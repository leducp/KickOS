#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Memory-domain enforcement gate: boot the `mpu_fault` image and assert the deliberate
# CROSS-DOMAIN write traps. An unprivileged domain-A thread writes its own granted
# region (must succeed), then writes domain B's region (must fault). Native for the
# sim, QEMU when QEMU_MACHINE is set.
#
# Registered only on an enforcing build; on a flat one the write completes, which is
# correct there and would (rightly) fail this gate.
#
# The banner alone is not the claim. Without the control marker AND the address pin
# below, a total grant failure (region A never granted at all) still passes: the fault
# then happens on the thread's OWN write, at a different address, and every marker the
# gate greps still appears.

set -u
. "$(dirname "$0")/lib/gate.sh"

elf="${1:?usage: check_mpu_fault.sh <mpu_fault.elf>}"

run_image "$elf"

if has "ERROR"; then
    fail "mpu_fault reported a failed setup or control arm"
fi
if has_e "did not fault|cross-domain write completed"; then
    fail "the cross-domain write was NOT trapped (enforcement inactive?)"
fi
# The CONTROL half must have run: the thread writing its OWN granted region and reading
# the value back is what separates "domain B is refused" from "region A was never
# granted", which faults earlier and prints the same banner.
if ! has "\[domain\] A: my region ok"; then
    fail "the control write never took effect (region A not granted?)"
fi

# Pin the trap to the address the app announced. This is what the banner cannot say:
# the two families report from different places (RISC-V and the sim take the
# kernel-reported path and name the task; ARM takes MemManage straight to
# kickos_armv7m_fault_report, which prints no name and only labels the dump "MPU FAULT"
# when the CFSR MMFSR byte is set), but both record the faulting address.
want="$(printf '%s\n' "$OUT" \
    | sed -n 's/.*\[domain\] expect fault at 0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$want" ]; then
    fail "the app never announced its expected fault address (faulted during setup?)"
fi
if ! has_e "MPU FAULT: task 'domainA'|=== MPU FAULT ==="; then
    fail "MPU FAULT marker missing (crash / hang / truncated run?)"
fi
got="$(reported_fault_addr)"
if [ -z "$got" ]; then
    fail "the fault report carries no address (KICKOS_PANIC_DUMP off?)"
fi
if [ "$((0x$got))" -ne "$((0x$want))" ]; then
    fail "trap at 0x$got, not at domain B's 0x$want (the wrong write faulted)"
fi

echo "PASS: the unprivileged cross-domain write trapped at 0x$got"
exit 0
