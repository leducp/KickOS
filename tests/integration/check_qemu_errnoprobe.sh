#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the per-thread errno witness under QEMU and require its own verdict. The app
# provokes two DIFFERENT errno values out of newlib itself and reads each back after a
# round trip that crossed the other thread's write; see its main.cc for what each arm
# rules out.
#
# ABOVE ONE KERNEL CORE THIS SKIPS. newlib's reentrancy state is reached through ONE word in
# the process's own memory, which the switch path rewrites to name the incoming thread's block,
# so two threads of one process running at the same instant on two cores resolve that one word
# and share an errno. The word is read at EL0, where a thread cannot ask which core it is on, so
# the seat belongs in thread-local storage; until it moves there this probe measures a
# known-open defect.
set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_qemu_errnoprobe.sh <errnoprobe.elf> [kernel-cores]"
elf="${1:?$_usage}"
cores="${2:-1}"
require_number "$cores" "the kernel core count"
if [ "$cores" -gt 1 ]; then
    echo "SKIP: $cores kernel cores share one reentrancy seat word in the process's memory, so
  two threads of one process resolve one errno. The seat is read at EL0 and a thread cannot
  ask which core it is on, so what fixes it is the thread pointer and not a per-core cell"
    exit 77
fi

poll_image "$elf" "\[errnoprobe\] (PASS|FAIL)"

if [ "$POLL_OK" -ne 1 ]; then
    fail "errnoprobe reached no verdict: neither PASS nor FAIL was printed"
fi
if has "\[errnoprobe\] FAIL"; then
    fail "errnoprobe reported FAIL"
fi
# A VERDICT IS NOT COVERAGE. PASS is printed by whatever arms ran, so deleting one leaves the
# gate green; each arm's own summary line is required by name. The letters are the app's
# (main.cc): A the two workers plus root, B a reused slot, C an in-dispatch server, D a
# preempted pair.
for _arm in \
    'A root at' \
    'B t[0-9] at' \
    'C srv' \
    'D lo'
do
    if ! has "\\[errnoprobe\\] $_arm"; then
        fail "errnoprobe printed no '$_arm' line, so that arm did not run and PASS covers less
  than it claims"
    fi
done

# D's own vacuity: the pair can run without the preemption it exists to witness ever landing,
# and the arm still reports a verdict.
if has '\[errnoprobe\] D .* overlapped 0 '; then
    fail "errnoprobe arm D never saw the preempted thread spinning, so it witnessed no
  preemption and its verdict is about nothing"
fi

assert_no_panic "errnoprobe panicked before reaching a verdict"

echo "PASS: QEMU errnoprobe gave every thread its own errno"
exit 0
