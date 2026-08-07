#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-dump gate: run the `fault` image (which executes an illegal instruction),
# and assert: the dump MARKER appears EXACTLY ONCE, and the process exits with the
# status that marker implies. The marker presence catches a dump that was enqueued
# into an armed console ring and lost (the C1 regression) rather than written
# synchronously; the exactly-once count catches a dump doubled by a re-pushed ring
# (the drain-reentrancy regression), and on an isolating backend a redirect that
# fired twice. Native run for the sim; QEMU (semihosting) when QEMU_MACHINE is set.
#
# The marker/status PAIR is the caller's, because which one is right is a property of
# the backend, not of this script: `fault` runs its illegal instruction from root, and
# root is unprivileged in every posture, so a backend that opted into fault isolation
# kills the thread ("THREAD FAULT", KOS_EXIT_FAULT) where the others panic ("HARD
# FAULT" / "SIM FAULT" / "RISC-V TRAP", 132 from kfault_terminate). Root is the only
# live thread here, so exit_current still ends the process either way.
#
# Coverage note: of the wired targets, only the sim actually ARMS a console ring
# (mps2/virt/nrf51 are polled semihosting). The ring-arming silicon boards (xmc4800,
# mk64f, rx72m, esp32-wroom, rp2040, sam3x8e, the stm32 fleet) have no QEMU model,
# so their armed-ring fault dump is validated by the manual HW flash pass, not here.

set -u
. "$(dirname "$0")/lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"
: "${SIM_TIMEOUT:=15}"

_usage="usage: check_fault_dump.sh <fault.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

run_image "$elf"

if has "did not fault"; then
    fail "the illegal instruction did not trap"
fi
count="$(printf '%s\n' "$OUT" | grep -c "$marker")"
if [ "$count" -eq 0 ]; then
    fail "fault-dump marker '$marker' missing: dump lost (enqueued into an undrained ring?)"
fi
if [ "$count" -ne 1 ]; then
    fail "fault-dump marker '$marker' appeared $count times: dump doubled (ring re-pushed?)"
fi
if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: fault dump present ('$marker') + exit $expect_status"
exit 0
