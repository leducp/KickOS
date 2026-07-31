#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-dump gate: run the `fault` image (which executes an illegal instruction),
# and assert: the arch fault reporter's dump MARKER appears EXACTLY ONCE, and the
# process exits with the fault status (132, from kfault_terminate). The marker
# presence catches a dump that was enqueued into an armed console ring and lost
# (the C1 regression) rather than written synchronously; the exactly-once count
# catches a dump doubled by a re-pushed ring (the drain-reentrancy regression).
# Native run for the sim; QEMU (semihosting) when QEMU_MACHINE is set.
#
# Coverage note: of the wired targets, only the sim actually ARMS a console ring
# (mps2/virt/nrf51 are polled semihosting). The ring-arming silicon boards -- xmc4800,
# mk64f, rx72m, esp32-wroom, rp2040, sam3x8e, the stm32 fleet -- have no QEMU model,
# so their armed-ring fault dump is validated by the manual HW flash pass, not here.

set -u
. "$(dirname "$0")/lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"
: "${SIM_TIMEOUT:=15}"

elf="${1:?usage: check_fault_dump.sh <fault.elf> <dump-marker>}"
marker="${2:?usage: check_fault_dump.sh <fault.elf> <dump-marker>}"
expect_status=132

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
    fail "expected exit $expect_status (kfault_terminate), got $RC"
fi
echo "PASS: fault dump present ('$marker') + exit $expect_status"
exit 0
