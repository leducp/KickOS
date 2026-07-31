#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Regression gate: a non-last thread that exits must not panic. Boot sched_exit
# (natively for the sim, on QEMU when QEMU_MACHINE is set) and assert the worker
# actually ran and exited AND that root ran past it. Requiring only the survival
# marker passes with the spawn deleted: root survives an exit that never happened.

set -u
. "$(dirname "$0")/lib/gate.sh"
: "${QEMU_TIMEOUT:=8}"
: "${SIM_TIMEOUT:=8}"

elf="${1:?usage: check_sched_exit.sh <sched_exit.elf>}"

run_image "$elf"

assert_no_panic "panic on non-last thread exit"
if ! has "worker: running"; then
    fail "the worker never ran (spawn refused or dropped?)"
fi
if ! has "worker: exiting"; then
    fail "the worker never reached its exit"
fi
if ! has "root: survived worker exit"; then
    fail "root did not survive the worker's exit"
fi

echo "PASS: non-last thread exit did not panic"
exit 0
