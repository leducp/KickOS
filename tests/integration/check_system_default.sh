#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The default system (docs/design-m10-target.md, section 8): an image linking KickOS::kernel and
# KickOS::system_default runs its plain C main as the default composition's `main` task, and the
# end of that task ends the system with the task's status, the init acting on it.
#
#   check_system_default.sh <image> <line main prints> <status> [<faulting thread>]
#
# The status is the half a main that never ran cannot fake: 124 is an init that never ended the
# system, and anything else is a status that did not travel from main through the init. Where
# main faults, the kernel's report names the faulting thread.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=20}"

USAGE="usage: check_system_default.sh <image> <line> <status> [<faulting thread>]"
image="${1:?$USAGE}"
line="${2:?$USAGE}"
status="${3:?$USAGE}"
faulting="${4:-}"

run_image "$image"

# Over the whole capture, once the run has ended.
assert_no_panic "a panic on the default system's path"
if has_e '^sysdefault: spinner (refused|never ran)'; then
    fail "main's spinner never spun, so the run witnesses a main that returned alone"
fi
if ! printf '%s\n' "$OUT" | grep -qxF "$line"; then
    fail "main's line '$line' never reached the console"
fi
if [ -n "$faulting" ]; then
    has "^$(thread_fault_re "$faulting")" || fail "no fault report names thread '$faulting'"
elif has '^=== THREAD FAULT ==='; then
    fail "a thread faulted"
fi
if [ "$RC" -eq 124 ]; then
    fail "the system never ended (timed out): the init did not act on main's end"
fi
if [ "$RC" -ne "$status" ]; then
    fail "the system ended with status $RC, not $status"
fi
echo "PASS: main printed its line and its task's end ended the system with status $status"
