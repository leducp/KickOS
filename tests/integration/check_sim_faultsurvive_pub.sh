#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-isolation witness ON A PUBLISHED CONSOLE: run faultsurvive_published, whose composition
# names the packaged simcon as stdout (a userspace driver owns the "wire"; see
# system/driver/sim/simcon/simcon.cc), and require the `survive' arm to hold there too.
#
# The claim is ORDERING through the driver's own queue. cap_console_deliver hands the record
# to the driver by popping it out of recv, so main's later line finds no parked receiver and
# parks in send_waiters instead; the driver emits the record, returns to recv, and only then
# takes main's line. An implementation that queued the record somewhere and left it for later
# satisfies presence and fails this.
#
# A real handover is a PREMISE here, tested across rather than proven: the route being real is
# sim_published_panic's negative assertion, where the app's kos_print witness must be ABSENT
# for the kernel chip path to be dark.
#
# usage: check_sim_faultsurvive_pub.sh <faultsurvive_published>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

APP="${1:?usage: check_sim_faultsurvive_pub.sh <faultsurvive_published>}"
[ -x "$APP" ] || fail "no faultsurvive_published image at $APP"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

printf '%s\n' "$OUT" | grep -q '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"

# The arm logic is check_faultsurvive.sh's, so the record, the survivor line, their order and
# the exit status are judged in one place. It re-runs the image, which is deterministic here.
"$(dirname "$0")/check_faultsurvive.sh" "$APP" survive sim terminated \
  || fail "the survive arm does not hold on a published console"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0 once main returned, got $RC"

echo "PASS: the kill record reaches the wire through a published userspace console, ahead of the survivor's own line"
