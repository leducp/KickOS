#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for a console driver that prints at its start, restarted: build conrestart, whose
# composition names the packaged simcon as stdout with one restart, the driver bounded to TWO
# served lines (-DKICKOS_SIMCON_EXIT_AFTER=2).
# simcon prints through kos_print before it serves. A thread of the task serving the console has
# no stdout, so that print takes the kernel console, which refuses it at once; were its stdout
# the endpoint it serves, the print would wait on its own receiver, and neither instance would
# ever be ready.
#
# A line parked on the console when its driver dies waits for the restart, which the init can
# still make, and reaches the restarted instance; a line once the init is out of restarts is
# refused.
#
# usage: check_sim_conrestart.sh <kickos-source-dir> <cmake>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }
FAILED_LINE='init: `simcon` failed to start'
UP_LINE='\[simcon\] driver up (host fd 1)'

scratch_dir

echo "== configuring the sim: console driver bounded to 2 messages =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SIMCON_EXIT_AFTER=2 >/dev/null ) \
  || fail "configure with the bounded console driver failed"

echo "== building conrestart =="
"$CMAKE" --build "$TMP/build" --target conrestart >/dev/null \
  || fail "conrestart build failed"

APP="$TMP/build/user/apps/common/conrestart/conrestart"
[ -x "$APP" ] || fail "conrestart binary not produced at $APP"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

[ "$(count_of "$FAILED_LINE")" -eq 0 ] \
  || fail "a start of the console driver failed: its print at start did not return"
[ "$(count_of "$UP_LINE")" -eq 2 ] \
  || fail "the console driver did not come up twice, first and restarted"
has '\[conrestart\] served by the first instance, its last line' \
  || fail "the first instance never carried main's lines"
[ "$(count_of '\[conrestart\] parked across the driver')" -eq 1 ] \
  || fail "the line parked at the driver's death did not reach the wire exactly once"
if has '\[conrestart\] past the last instance'; then
    fail "a line written once the init was out of restarts reached the wire"
fi
_last_up="$(printf '%s\n' "$OUT" | grep -n "$UP_LINE" | tail -n1 | cut -d: -f1)"
_parked="$(printf '%s\n' "$OUT" | grep -n 'parked across the driver' | head -n1 | cut -d: -f1)"
[ -n "$_last_up" ] && [ -n "$_parked" ] && [ "$_parked" -gt "$_last_up" ] \
  || fail "the parked line reached the wire ahead of the restarted instance"
if has 'kernel console diagnostic'; then
    fail "the driver's print at start reached the wire from a console it owns"
fi
# Each instance's death gives the console back to the kernel, which says so.
[ "$(count_of 'kernel: console driver task ended, console reclaimed')" -eq 2 ] \
  || fail "the kernel's line on a driver's death did not reach the wire once per death"
[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC (conrestart/main.cc names each status)"

echo "PASS: a console driver that prints at its start comes up, and comes up again restarted"
