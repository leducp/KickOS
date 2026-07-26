#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the POST-PUBLISH console posture: build the sim with the publishing
# service list (kickos_services_sim -- a userspace console driver owns the "wire", see
# system/init/service_list_sim.cc) and require the selftest TAP stream to arrive over
# the DRIVER, clean.
#
# Why this needs its own build: KICKOS_SERVICE_LIST selects one provider per image, so
# the two console postures cannot coexist in a single tree. Every other sim and QEMU
# gate runs kickos_services_none, where the kernel keeps the console, cap index 0 is
# unseated and the endpoint route is never touched -- which is exactly how M4.5 shipped
# a console handover that silenced the whole test harness on xmc4800-relax and
# frdmk64f without a single gate going red. This gate is the missing half.
#
# The load-bearing assertion is the NEGATIVE one: the console driver's own kos::print
# banner must be ABSENT. It goes to the kernel debug console, which a published board
# drops by design -- so its absence proves the handover really happened and the TAP
# stream we just read came through the endpoint, not through a silent fallback. Without
# it, a regression that skipped the publish entirely would still pass here.
#
# usage: check_sim_published.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

fail() { echo "FAIL: $1"; exit 1; }
# grep as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim with the publishing service list =="
# Via --preset so this tree matches the ordinary sim build exactly (generator, build
# type, warning flags incl. -Werror, KICKOS_ENABLE_SELFTEST) and still inherits
# CFLAGS/CXXFLAGS from the environment -- which is how the sim-ubsan job's sanitizer
# flags reach it too.
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim >/dev/null ) \
  || fail "configure with kickos_services_sim failed"

echo "== building selftest =="
"$CMAKE" --build "$TMP/build" --target selftest >/dev/null \
  || fail "selftest build failed"

APP="$TMP/build/user/apps/common/selftest/selftest"
[ -x "$APP" ] || fail "selftest binary not produced at $APP"

echo "== running selftest against the published console =="
set +e
OUT="$("$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

[ "$RC" -eq 0 ] || fail "selftest exited $RC (a failing test, or a truncated run)"

has '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"
if has 'kos::print diagnostic'; then
    fail "the kernel debug console is STILL live -- no real handover, so this gate proved nothing"
fi
has '^# tap route: stdout endpoint' || fail "TAP did not take the published endpoint route"
has '^1\.\.'    || fail "no TAP plan line: the whole stream was dropped (the M4.5 regression)"
if has 'not ok'; then
    fail "a TAP test reported not ok on the published console"
fi
has '^# skipped: 0$'      || fail "unexpected skip(s) on the sim, whose pools host the whole suite"
has '^# all tests passed$' || fail "TAP completion marker missing (truncated run?)"

echo "PASS: the full TAP stream is observable over a published userspace console"
