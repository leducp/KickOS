#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Three regression gates on one boot of sched_exit (natively for the sim, on QEMU when
# QEMU_MACHINE is set).
#
# 1. A non-last thread that exits must not panic. Assert the worker actually ran and
# exited AND that root ran past it: requiring only the survival marker passes with the
# spawn deleted, root surviving an exit that never happened.
#
# 2. Wait-until-last releases the parked root when its child exits, and refuses a NON-ROOT
# caller -KOS_EPERM. Both markers are required: the refusal alone would pass with root's
# own wait never satisfied, and the release alone would pass with the root-only rule gone.
# A never-released root shows up as the 124 timeout below.
#
# 3. Root's own exit must end the SYSTEM while a child is still alive. arch_shutdown
# forwards the status, so the exit code IS the witness: 7 means root's exit reached
# kickos_terminate carrying its argument, 124 means the system ran on with a dead init
# and the image had to be killed.

set -u
. "$(dirname "$0")/lib/gate.sh"
: "${QEMU_TIMEOUT:=8}"
: "${SIM_TIMEOUT:=8}"

elf="${1:?usage: check_sched_exit.sh <sched_exit.elf>}"

run_image "$elf"

assert_no_panic "panic on thread exit"
if ! has "worker: running"; then
    fail "the worker never ran (spawn refused or dropped?)"
fi
if ! has "worker: exiting"; then
    fail "the worker never reached its exit"
fi
if ! has "root: survived worker exit"; then
    fail "root did not survive the worker's exit"
fi
if has "wait_last spawn refused"; then
    fail "the non-root waiter could not be spawned; the wait-until-last arm witnessed nothing"
fi
if has "child: wait_last NOT refused"; then
    fail "a non-root wait-until-last caller was accepted instead of -KOS_EPERM"
fi
if ! has "child: wait_last refused"; then
    fail "the non-root caller never reported (it parked instead of being refused?)"
fi
if ! has "root: last thread standing"; then
    fail "root's wait-until-last was not released by its child's exit"
fi
if has "parked spawn refused"; then
    fail "the never-exiting child was refused; root's exit arm witnessed nothing"
fi
if ! has "root: exiting with a child alive"; then
    fail "root never reached its own exit"
fi
if [ "$RC" -eq 124 ]; then
    fail "root's exit left the system running with a dead init (timed out)"
fi
if [ "$RC" -ne 7 ]; then
    fail "root's exit shut down with status $RC, not the 7 it passed"
fi

echo "PASS: a non-last thread exit did not panic, wait-until-last released root and refused a non-root caller, and root's exit shut the system down"
exit 0
