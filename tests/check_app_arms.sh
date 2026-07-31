#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict gate for a self-asserting ARM-COUNTING app: one that emits `[<prefix>] ok -
# <arm>` per arm, `[<prefix>] ERROR: <arm>` per failure, and ends with
# `[<prefix>] PASS (<n> arms)`. Boots on QEMU when QEMU_MACHINE is set, natively
# otherwise, so the sim and the QEMU boards are held to one standard.
#
# The count must be MET EXACTLY, not merely reached: too few means an arm was deleted
# or the run was cut short, too many means the posture changed under the gate (an
# armv6m no-ring board reporting the full armv7m set, say) without the expectation
# being updated. The caller owns the number because only its CMakeLists knows the
# board's posture. And the verdict's own tally must agree with the lines on the wire:
# those come from different code paths (a counter vs one emit per arm), so a mismatch
# means output was lost between them and the count cannot be trusted.
#
# usage: check_app_arms.sh <elf> <prefix> <exact-arm-count> [must-be-absent...]

set -u
. "$(dirname "$0")/lib/gate.sh"

elf="${1:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
prefix="${2:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
want_arms="${3:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
shift 3

run_image "$elf"

if has_e "\[$prefix\] (ERROR|FAIL)"; then
    fail "$prefix reported a failed arm"
fi
# The verdict prints before main returns, and that return can still panic (rootauth's
# kos_shutdown is gated on KOS_AUTH_SYSTEM), so the marker alone is not the verdict.
assert_no_panic "$prefix panicked (before or after its verdict)"
for absent in "$@"; do
    if has "$absent"; then
        fail "$prefix printed '$absent'"
    fi
done

arms="$(printf '%s\n' "$OUT" | grep -c "\[$prefix\] ok - ")"
if [ "$arms" -ne "$want_arms" ]; then
    fail "$prefix reported $arms arm(s), expected exactly $want_arms"
fi
if ! has "\[$prefix\] PASS ($want_arms arms)"; then
    fail "$prefix verdict does not claim $want_arms arms (crash / hang / output lost?)"
fi

echo "PASS: $prefix clean ($arms arms)"
exit 0
