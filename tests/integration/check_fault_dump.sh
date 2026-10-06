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
# fired twice. Native run for the sim; QEMU (semihosting) when QEMU_MACHINE is set; with
# KOS_CAPTURE a silicon capture, which carries no exit status.
#
# The "did not fault" clause is an absence and weak on its own, which above one core is all a
# grep can be. It is corroborated: a run that did not fault carries no marker and does not exit
# with the status the marker implies, and both of those are asserted positively below. The EXIT
# STATUS is the half no shuffle can reach, the emulator reporting it rather than the wire.
#
# ABOVE ONE CORE THE MARKER CAN ARRIVE SPLIT: arch_console_write is a byte-at-a-time device loop
# under no lock, so a core printing a status line lands it INSIDE another's line character by
# character. The byte-exact match is tried first and a split is tolerated only where the image
# reported more than one core, through require_on_wire, which reports every split it allows with
# its span. The count ABOVE ONE stays byte-exact: an interleave hides a marker and can never
# manufacture a second one. This gate reads no field off the wire, so nothing here is a VALUE a
# bounded in-order match would have to guess at.
#
# Which marker/status PAIR is right is a property of the backend, so the caller passes it in.
# `fault` runs its illegal instruction from main, the default composition's task, which is
# unprivileged in every posture, so a backend that opted into fault isolation kills the
# thread ("THREAD FAULT", KOS_EXIT_FAULT, which ends main's task and the system through the
# init) where the others panic ("HARD FAULT" / "SIM FAULT" / "RISC-V TRAP", 132 from
# kfault_terminate).
#
# Coverage: the sim is the one wired target that ARMS a console ring, so it is the armed-ring
# path this gate witnesses; the other wired targets are polled semihosting. The ring-arming
# silicon boards are validated by the manual HW flash pass.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"
: "${SIM_TIMEOUT:=15}"

_usage="usage: check_fault_dump.sh <fault.elf> <dump-marker> <expect-status>
       KOS_CAPTURE=<log> check_fault_dump.sh <board-build> <kickos-source> <cmake> <dump-marker>"
if judging_capture; then
    shift 3
    marker="${1:?$_usage}"
    judge_capture fault
else
    elf="${1:?$_usage}"
    marker="${2:?$_usage}"
    expect_status="${3:?$_usage}"
    run_image "$elf"
fi

ANNOUNCE='[fault] executing an illegal instruction'
require_on_wire "$ANNOUNCE" "the fault app never announced its instruction" announce
if has "did not fault"; then
    cfail faulted "the illegal instruction did not trap"
fi
require_single_marker "$marker" "dump lost (enqueued into an undrained ring?)" \
    "dump doubled (ring re-pushed?)"
# Ordered only where both lines arrived whole: a split line has no line number of its own.
_said="$(printf '%s\n' "$OUT" | grep -nF -- "$ANNOUNCE" | head -n1 | cut -d: -f1)"
_dump="$(printf '%s\n' "$OUT" | grep -nF -- "$marker" | head -n1 | cut -d: -f1)"
if [ -n "$_said" ] && [ -n "$_dump" ] && [ "$_dump" -le "$_said" ]; then
    cfail order "the dump at line $_dump precedes the app's announcement at line $_said, so it is
  not this instruction's"
fi
status_clause "the fault ended the system with the status its marker implies" "${expect_status:-}"
if judging_capture; then
    echo "PASS: fault dump present ('$marker') once"
    exit 0
fi
echo "PASS: fault dump present ('$marker') + exit $expect_status"
exit 0
