#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Two regression gates on one boot of sched_exit on the default system (natively for the sim,
# on QEMU when QEMU_MACHINE is set).
#
# 1. A non-last thread that exits must not panic. Assert the worker actually ran and
# exited AND that main ran past it: requiring only the survival marker passes with the
# spawn deleted, main surviving an exit that never happened.
#
# 2. Main's own exit must end the SYSTEM while a child is still alive. The init forwards the
# status of the task the composition `ends` on, so the exit code IS the witness: 7 means
# main's exit reached the init's shutdown carrying its argument, 124 means the system ran on
# and the image had to be killed.

set -u
. "$(dirname "$0")/../lib/gate.sh"
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
if ! has "main: survived worker exit"; then
    fail "main did not survive the worker's exit"
fi
if has "parked spawn refused"; then
    fail "the never-exiting child was refused; main's exit arm witnessed nothing"
fi
if ! has "main: exiting with a child alive"; then
    fail "main never reached its own exit"
fi
if [ "$RC" -eq 124 ]; then
    fail "main's exit left the system running (timed out)"
fi
if [ "$RC" -ne 7 ]; then
    fail "main's exit shut down with status $RC, not the 7 it passed"
fi

echo "PASS: a non-last thread exit did not panic, and main's exit with a child alive shut the system down with its status"
exit 0
