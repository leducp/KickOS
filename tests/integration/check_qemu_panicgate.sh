#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU kos_panic wire gate: boot a `panicgate` image and assert the expected panic line
# reached the wire. This is the positive assertion, where check_app_arms.sh's absence one
# passes trivially when the panic path is never taken.
#
# $2 is the literal line expected after the kernel's trusted banner; $3, when non-empty,
# is a literal that must NOT appear (the tail a truncation drops). A FAULT banner is a
# failure even when the expected line is also present: the panic must come from the
# syscall, not from the kernel dereferencing the caller's pointer.
#
# ABOVE ONE CORE THE EXPECTED LINE CAN ARRIVE SPLIT, and that is permitted behaviour rather
# than a defect: arch_console_write is a byte-at-a-time device loop under no lock, so two harts
# emit into one wire with no per-line atomicity anywhere in the kernel. A peer starting the app
# while the boot hart prints its status lines therefore lands one of them INSIDE this line.
# The strict match is tried first and a split is tolerated only by deleting the KNOWN kernel
# status lines and rejoining; a line absent for any other reason still fails, and a tolerated
# split is REPORTED so it never passes silently.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_panicgate.sh <panicgate.elf> <expected-line> [absent]}"
expect="${2:?usage: check_qemu_panicgate.sh <panicgate.elf> <expected-line> [absent]}"
absent="${3:-}"

need_qemu_machine
run_image "$elf"

if has "\[panicgate\] ERROR"; then
    fail "kos_panic returned to the caller"
fi
# The arm must have been reached, so a panic cannot be credited to an earlier trap
# during ctors or bring-up.
if ! has "\[panicgate\] case"; then
    fail "the app never reached its kos_panic call"
fi
if has_e "=== (HARD|MPU|BUS) FAULT|=== RISC-V TRAP|MPU FAULT: thread"; then
    fail "the kernel faulted instead of refusing the message pointer"
fi
if ! printf '%s\n' "$OUT" | grep -qF -- "$expect"; then
    # The only concurrent writer here is the kernel's own boot reporting, whose lines are
    # matched exactly rather than by a wildcard: anything else stays a split this will not
    # paper over.
    joined="$(printf '%s\n' "$OUT" \
        | sed -e 's/# smp: [0-9]\{1,\} core(s) online//g' \
              -e 's/# smp sched: [0-9]\{1,\} core(s) in the scheduler//g' \
              -e 's/# doorbell: [0-9]\{1,\} core(s) answered, rounds 0x[0-9a-f]\{1,\}//g' \
        | tr -d '\n')"
    if printf '%s' "$joined" | grep -qF -- "$expect"; then
        echo "   TOLERATED A SPLIT: the expected line reached the wire broken by a kernel
   status line, which two harts on one unlocked device wire may do at any byte"
    else
        fail "expected panic line missing: $expect"
    fi
fi
if [ -n "$absent" ] && printf '%s\n' "$OUT" | grep -qF -- "$absent"; then
    fail "text that must not reach the wire is present: $absent"
fi

echo "PASS: $expect"
exit 0
