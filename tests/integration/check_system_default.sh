#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The default system (docs/design-m10-target.md, section 8): an image linking KickOS::kernel and
# KickOS::system_default runs its plain C main as the default composition's `main` task, and the
# end of that task ends the system with the task's status, the init acting on it.
#
#   check_system_default.sh <image> <line main prints> <status> [<faulting thread>]
#   KOS_CAPTURE=<log> check_system_default.sh <board-build> <kickos-source> <cmake> <line> <status>
#     [<faulting thread>]
#
# The status is the half a main that never ran cannot fake: 124 is an init that never ended the
# system, and anything else is a status that did not travel from main through the init. Where
# main faults, the kernel's report names the faulting thread. With KOS_CAPTURE, the last boot of
# a silicon capture is judged instead, by every clause but the status, which a capture does not
# carry.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=20}"

USAGE="usage: check_system_default.sh <image> <line> <status> [<faulting thread>]"
if judging_capture; then
    shift 3
    judge_capture sysdefault
else
    image="${1:?$USAGE}"
    shift
    run_image "$image"
fi
line="${1:?$USAGE}"
status="${2:?$USAGE}"
faulting="${3:-}"

# Over the whole capture, once the run has ended.
assert_no_panic "a panic on the default system's path"
if has_e '^sysdefault: spinner (refused|never ran)'; then
    cfail spinner "main's spinner never spun, so the run witnesses a main that returned alone"
fi
if ! printf '%s\n' "$OUT" | grep -qxF "$line"; then
    cfail line "main's line '$line' never reached the console"
fi
if [ -n "$faulting" ]; then
    _line_at="$(printf '%s\n' "$OUT" | grep -nxF -- "$line" | head -n1 | cut -d: -f1)"
    _fault_at="$(printf '%s\n' "$OUT" | grep -n "^$(thread_fault_re "$faulting")" | head -n1 | cut -d: -f1)"
    [ -n "$_fault_at" ] || cfail fault "no fault report names thread '$faulting'"
    if [ "$_fault_at" -le "$_line_at" ]; then
        cfail order "the fault report at line $_fault_at precedes main's line at $_line_at"
    fi
elif has '^=== THREAD FAULT ==='; then
    cfail fault "a thread faulted"
fi
status_clause "main's task ending ended the system with status $status" "$status"
if judging_capture; then
    echo "PASS: main printed its line, with no panic and no fault but the one it owed"
    exit 0
fi
echo "PASS: main printed its line and its task's end ended the system with status $status"
