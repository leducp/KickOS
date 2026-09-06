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
# ABOVE ONE CORE EVERY LINE HERE CAN ARRIVE SPLIT, and that is permitted behaviour rather
# than a defect: arch_console_write is a byte-at-a-time device loop under no lock, so two harts
# emit into one wire with no per-line atomicity anywhere in the kernel. A peer starting the app
# while the boot hart prints its status lines lands one of them INSIDE the app's line or inside
# the panic line, and the collision is CHARACTER BY CHARACTER rather than a whole line inserted
# at a line break, so deleting the known status lines and rejoining does not recover it. The
# strict match is tried first; a split is tolerated only above one core and only through
# wire_has, whose span bound is what one status line can contribute, so a literal absent for
# any other reason still fails. Every tolerated split is REPORTED.
#
# THE THREE ABSENCE ASSERTIONS BELOW ARE NOT SHUFFLE-PROOF AND CANNOT BE: a shuffle hides text
# from grep, so it can only make them pass. Each is corroborated by a positive assertion the
# same defect also breaks. kos_panic returning to the caller, or the kernel faulting on the
# message pointer, leaves no panic line for the expected-line check. A truncation that did not
# happen leaves the dropped tail on the wire AND leaves the expected line without its marker.

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
require_on_wire "[panicgate] case" "the app never reached its kos_panic call"
if has_e "=== (HARD|MPU|BUS) FAULT|=== RISC-V TRAP|MPU FAULT: thread"; then
    fail "the kernel faulted instead of refusing the message pointer"
fi
require_on_wire "$expect" "expected panic line missing: $expect"
if [ -n "$absent" ] && printf '%s\n' "$OUT" | grep -qF -- "$absent"; then
    fail "text that must not reach the wire is present: $absent"
fi

echo "PASS: $expect"
exit 0
