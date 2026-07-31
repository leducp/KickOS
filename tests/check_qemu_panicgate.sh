#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU kos_panic wire gate: boot a `panicgate` image and assert the expected panic line
# reached the wire. The positive peer of check_app_arms.sh's absence assertion,
# which passes trivially when the panic path is never taken.
#
# $2 is the literal line expected after the kernel's trusted banner; $3, when non-empty,
# is a literal that must NOT appear (the tail a truncation drops). A FAULT banner is a
# failure even when the expected line is also present: the panic must come from the
# syscall, not from the kernel dereferencing the caller's pointer.

set -u
. "$(dirname "$0")/lib/gate.sh"

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
if has_e "=== (HARD|MPU|BUS) FAULT|=== RISC-V TRAP|MPU FAULT: task"; then
    fail "the kernel faulted instead of refusing the message pointer"
fi
if ! printf '%s\n' "$OUT" | grep -qF -- "$expect"; then
    fail "expected panic line missing: $expect"
fi
if [ -n "$absent" ] && printf '%s\n' "$OUT" | grep -qF -- "$absent"; then
    fail "text that must not reach the wire is present: $absent"
fi

echo "PASS: $expect"
exit 0
