#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for a console driver that prints at its start, restarted: build conrestart, whose
# composition names the packaged simcon as stdout with one restart, the driver bounded to TWO
# served messages (-DKICKOS_SIMCON_EXIT_AFTER=2), the handover probe and one line of main's.
# simcon prints through kos_print before it serves. A thread of the task serving the console has
# no stdout, so that print takes the kernel console, which drops it at once; were its stdout the
# endpoint it serves, the print would wait on its own receiver, and neither instance would take
# its handover probe.
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
has '\[conrestart\] served by the first instance' \
  || fail "the first instance never carried main's line"
has '\[conrestart\] served by the restarted instance' \
  || fail "main's line after the restart never reached the wire"
if has 'kernel console diagnostic'; then
    fail "the driver's print at start reached the wire from a console it owns"
fi
[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC"

echo "PASS: a console driver that prints at its start comes up, and comes up again restarted"
