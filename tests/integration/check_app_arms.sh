#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Verdict gate for a self-asserting ARM-COUNTING app: one that emits `[<prefix>] ok -
# <arm>` per arm, `[<prefix>] ERROR: <arm>` per failure, and ends with
# `[<prefix>] PASS (<n> arms)`. Boots on QEMU when QEMU_MACHINE is set, natively otherwise, and
# reads a silicon capture with KOS_CAPTURE.
#
# The count must be MET EXACTLY, not merely reached: too few means an arm was deleted or the
# run was cut short, too many means the posture changed under the gate without the
# expectation being updated. The caller owns the number because only its CMakeLists knows the
# board's posture. The verdict's own tally must also agree with the lines on the wire: those
# come from different code paths, a counter against one emit per arm, so a mismatch means
# output was lost between them and the count cannot be trusted.
#
#   check_app_arms.sh <elf> <prefix> <exact-arm-count> [must-be-absent...]
#   KOS_CAPTURE=<log> check_app_arms.sh <board-build> <kickos-source> <cmake> <prefix> <exact-arm-count>
#     [must-be-absent...]

set -u
. "$(dirname "$0")/../lib/gate.sh"

if judging_capture; then
    shift 3
else
    elf="${1:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
    shift
fi
prefix="${1:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
want_arms="${2:?usage: check_app_arms.sh <elf> <prefix> <arms> [absent...]}"
shift 2
if judging_capture; then
    judge_capture "$prefix"
else
    run_image "$elf"
fi

if has_e "\[$prefix\] (ERROR|FAIL)"; then
    cfail error "$prefix reported a failed arm"
fi
# The verdict prints before main returns, and the end that follows can still panic,
# so the marker alone is not the verdict.
assert_no_panic "$prefix panicked (before or after its verdict)"
for absent in "$@"; do
    if has "$absent"; then
        cfail absent "$prefix printed '$absent'"
    fi
done

arms="$(printf '%s\n' "$OUT" | grep -c "\[$prefix\] ok - ")"
if [ "$arms" -ne "$want_arms" ]; then
    cfail arms "$prefix reported $arms arm(s), expected exactly $want_arms"
fi
if ! has "\[$prefix\] PASS ($want_arms arms)"; then
    cfail verdict "$prefix verdict does not claim $want_arms arms (crash / hang / output lost?)"
fi

echo "PASS: $prefix clean ($arms arms)"
exit 0
