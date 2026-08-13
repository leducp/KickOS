#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-isolation witness ON A PUBLISHED CONSOLE: build the sim with the publishing service
# list (kickos_services_sim, where a userspace driver owns the "wire"; see
# system/init/sim/service_list.cc) and require the `survive' arm to hold there too.
#
# What this adds over sim_published_panic, which also runs a fault on a published console:
# ORDERING through the driver's own queue. cap_console_deliver hands the record to the
# driver by popping it out of recv, so root's later line finds no parked receiver and parks
# in send_waiters instead; the driver therefore emits the record, returns to recv, and only
# then takes root's line. An implementation that merely queued the record somewhere and left
# it for later would satisfy presence and fail this.
#
# It is deliberately NOT the anti-vacuity gate for the route being real: that is
# sim_published_panic's negative assertion (the app's kos_print witness must be ABSENT,
# which is what proves the kernel chip path is dark). This gate assumes a real handover and
# tests what happens across it.
#
# usage: check_sim_faultsurvive_pub.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="${1:?usage: check_sim_faultsurvive_pub.sh <src> <cmake>}"
CMAKE="${2:-cmake}"

fail() { echo "FAIL: $1"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim with the publishing service list =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim >/dev/null ) \
  || fail "configure with kickos_services_sim failed"

echo "== building faultsurvive =="
"$CMAKE" --build "$TMP/build" --target faultsurvive >/dev/null \
  || fail "faultsurvive build failed"

APP="$TMP/build/user/apps/common/faultsurvive/faultsurvive"
[ -x "$APP" ] || fail "faultsurvive binary not produced at $APP"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

printf '%s\n' "$OUT" | grep -q '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"

# The generic arm logic is check_faultsurvive.sh's and is not copied here: presence of the
# kill record, presence of the survivor line, the record STRICTLY BEFORE it, no panic, and
# exit 0. It re-runs the image, which is deterministic on the host.
"$(dirname "$0")/check_faultsurvive.sh" "$APP" survive sim \
  || fail "the survive arm does not hold on a published console"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0 once root returned, got $RC"

echo "PASS: the kill record reaches the wire through a published userspace console, ahead of the survivor's own line"
