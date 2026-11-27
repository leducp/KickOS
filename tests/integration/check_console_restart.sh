#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The console restart witness (docs/design-m10-target.md, section 8): the system of
# tests/integration/console_witness under <composition>, built against the build's installed
# package, whose catalogue carries tests/drivers, and run under QEMU. testpl011 serves stdout over
# the board's console PL011, five writes per instance, and is restarted once; a writer prints
# thirty numbered lines and its return ends the system.
#
# The capture passes on each of the thirty lines at most once and in order, which no handover may
# repeat; on two instances, each marking its start and its end on the wire itself, the second
# starting after the first ended; on a line of the writer between each instance's marks, which
# only that instance can have written; on NO line of the writer after the second end, the kernel
# console given back at that death carrying no user line; on the init's report of that death
# reaching no wire either; and on the system ending with the writer's status, 0, which says its
# lines past the driver's end were refused, each with ECONNREFUSED.
#
#   check_console_restart.sh <kickos-build> <kickos-source> <cmake> <composition>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_console_restart.sh <kickos-build> <kickos-source> <cmake> <composition>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
SYSTEM="${4:?$USAGE}"
need_qemu_machine
[ -f "$SYSTEM" ] || fail "no composition at $SYSTEM"

LINES=30
SERVING='testpl011: serving'
EXITING='testpl011: served its writes, exiting'

scratch_dir

echo "== building the console witness against the installed package =="
package_image "$KICKOS_BUILD" "$CMAKE" "$KICKOS_SRC/tests/integration/console_witness" console_witness \
    -DKICKOS_WITNESS_SYSTEM="$SYSTEM"

echo "== running the console witness =="
run_image "$IMAGE"
assert_no_panic "a panic in the console witness"
if has '^=== THREAD FAULT ==='; then
    fail "a thread faulted"
fi

for _mark in "$SERVING" "$EXITING"; do
    count_literal "$_mark"
    [ "$KOS_COUNT" -eq 2 ] || fail "'$_mark' reached the wire $KOS_COUNT time(s), not once per instance"
done
# The init's report of the first death is served by the restarted instance; the report of the
# second has no console to go to.
count_literal 'init: `console` is dead and released'
[ "$KOS_COUNT" -eq 1 ] || fail "the init reported $KOS_COUNT death(s) of the console driver, not the first alone"
# The kernel's line on each death, on the device each reclaim gave back.
count_literal 'kernel: console driver task ended, console reclaimed'
[ "$KOS_COUNT" -eq 2 ] || fail "the kernel's line on a driver's death reached the wire $KOS_COUNT time(s), not once per death"

after 1 "$SERVING" "the first instance's start"
SERVED1=$AT
after "$SERVED1" "$EXITING" "the first instance's end"
EXITED1=$AT
after "$EXITED1" "$SERVING" "the restarted instance's start"
SERVED2=$AT
after "$SERVED2" "$EXITING" "the restarted instance's end"
EXITED2=$AT
after "$SERVED2" 'init: `console` is dead and released' "the init's report of the first death"
[ "$AT" -lt "$EXITED2" ] || fail "the init's report of the first death was not served by the restarted instance"
after "$EXITED1" 'kernel: console driver task ended, console reclaimed' "the kernel's line on the first death"
[ "$AT" -lt "$SERVED2" ] || fail "the kernel's line on the first death came after the restart"
after "$EXITED2" 'kernel: console driver task ended, console reclaimed' "the kernel's line on the second death"

# The line of `conwit: line <k> out`, or 0.
line_of() { # <k>
    printf '%s\n' "$OUT" | awk -v want="conwit: line $1 out" '$0 == want { print NR; exit }'
}

_prev=0
_in1=0
_in2=0
_after=0
_k=1
while [ "$_k" -le "$LINES" ]; do
    count_literal "conwit: line $_k out"
    [ "$KOS_COUNT" -le 1 ] || fail "line $_k of the writer reached the wire $KOS_COUNT time(s)"
    if [ "$KOS_COUNT" -eq 0 ]; then
        _k=$((_k + 1))
        continue
    fi
    _at="$(line_of "$_k")"
    [ -n "$_at" ] || fail "line $_k of the writer is not a whole line of the capture"
    [ "$_at" -gt "$_prev" ] || fail "line $_k of the writer precedes line $((_k - 1))"
    if [ "$_at" -gt "$SERVED1" ] && [ "$_at" -lt "$EXITED1" ]; then
        _in1=$((_in1 + 1))
    fi
    if [ "$_at" -gt "$SERVED2" ] && [ "$_at" -lt "$EXITED2" ]; then
        _in2=$((_in2 + 1))
    fi
    if [ "$_at" -gt "$EXITED2" ]; then
        _after=$((_after + 1))
    fi
    _prev=$_at
    _k=$((_k + 1))
done
[ "$_in1" -gt 0 ] || fail "the first instance served none of the writer's lines"
[ "$_in2" -gt 0 ] || fail "the restarted instance served none of the writer's lines"
[ "$_after" -eq 0 ] || fail "$_after line(s) of the writer reached the console the kernel took back at the restarted instance's end"
if [ "$RC" -eq 124 ]; then
    fail "the system never ended (timed out)"
fi
[ "$RC" -ne 1 ] || fail "no line of the writer was refused, so none was written past the driver's end"
[ "$RC" -ne 2 ] || fail "a line of the writer was refused with an errno other than ECONNREFUSED"
if [ "$RC" -ne 0 ]; then
    fail "the system ended with status $RC, not 0"
fi
echo "PASS: the console driver served $_in1 line(s), died, was restarted through a second handover and \
served $_in2, and every line past its end was refused; no line twice, in order"
