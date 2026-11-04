#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# wallwait (user/apps/common/wallwait) booted under QEMU: a thread's system_clock wait an hour
# ahead, through a condition variable and through a timed mutex, ends timed out after main sets
# the wall clock a day ahead, and within ENDED_MAX_NS of the set. A wait that saw the set only at
# a slice of its own returns before the set, or a slice late. The system ends with status 0.
#
#   check_wallwait.sh <image>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

# Far under an hour, and over a host stall of the emulator.
ENDED_MAX_NS=1000000000

run_image "${1:?usage: check_wallwait.sh <image>}"
[ "$RC" = "0" ] || fail "wallwait ended with status $RC"
if has_e "panic|PANIC"; then
    fail "a panic in wallwait"
fi
has_f "[wallwait] done" || fail "wallwait printed no end line"

_line='^\[wallwait\] \(.*\) an hour ahead: a set a day ahead answered \(-*[0-9][0-9]*\), timed out \([a-z]*\), ended \(-*[0-9][0-9]*\) ns after the set$'
for _what in "condition_variable wait_until" "timed_mutex try_lock_until"; do
    L="$(printf '%s\n' "$OUT" | grep -aF "[wallwait] $_what an hour ahead: ")"
    [ -n "$L" ] || fail "no line for the $_what arm"
    SET="$(printf '%s\n' "$L" | sed -n "s/$_line/\2/p")"
    TIMED="$(printf '%s\n' "$L" | sed -n "s/$_line/\3/p")"
    AFTER="$(printf '%s\n' "$L" | sed -n "s/$_line/\4/p")"
    [ -n "$AFTER" ] || fail "the $_what line reads '$L'"
    [ "$SET" = "0" ] || fail "the wall-clock set of the $_what arm answered $SET"
    [ "$TIMED" = "yes" ] || fail "the $_what wait ended without timing out, $AFTER ns after the set"
    if [ "$AFTER" -lt 0 ]; then
        fail "the $_what wait ended $AFTER ns after the set, before it"
    fi
    if [ "$AFTER" -gt "$ENDED_MAX_NS" ]; then
        fail "the $_what wait ended $AFTER ns after the set, more than $ENDED_MAX_NS ns"
    fi
done
echo "PASS: a system_clock wait an hour ahead ended timed out promptly after a set a day ahead"
