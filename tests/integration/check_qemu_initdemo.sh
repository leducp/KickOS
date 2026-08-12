#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU regression gate (M4.3): a root pre-publish printf must not poison a
# post-publish worker's stdout route. Boot initdemo on QEMU via semihosting. The
# console is DARK after publish, so the verdict rides the EXIT STATUS: initdemo
# returns 0 iff the software console driver received exactly the worker's payload
# byte count, else 1. arch_shutdown forwards that via semihosting SYS_EXIT_EXTENDED,
# so QEMU's process exit code IS the verdict.
#
# PRE-fix (sticky process-wide probe): worker bytes bypass the endpoint -> count 0
# -> exit 1 -> FAIL. POST-fix (per-invocation re-probe): exit 0 -> PASS.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

elf="${1:?usage: check_qemu_initdemo.sh <initdemo.elf>}"

need_qemu_machine
run_image "$elf"

assert_no_panic "panic/fault during initdemo"
if [ "$RC" -eq 124 ]; then
    fail "initdemo timed out (no exit status forwarded)"
fi
if [ "$RC" -ne 0 ]; then
    fail "initdemo exit status $RC (worker bytes did not reach the endpoint)"
fi

echo "PASS: post-publish worker's stdout reached the endpoint (per-thread route)"
exit 0
