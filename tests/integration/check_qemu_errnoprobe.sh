#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the per-thread errno witness under QEMU and require its own verdict. The app
# provokes two DIFFERENT errno values out of newlib itself and reads each back after a
# round trip that crossed the other thread's write; see its main.cc for what each arm
# rules out.
set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_errnoprobe.sh <errnoprobe.elf>}"

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
