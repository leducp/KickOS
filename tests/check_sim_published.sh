#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the POST-PUBLISH console posture: build the sim with the publishing
# service list (kickos_services_sim: a userspace console driver owns the "wire", see
# system/init/sim/service_list.cc) and require the selftest TAP stream to arrive over
# the DRIVER, clean.
#
# Why this needs its own build: KICKOS_SERVICE_LIST selects one provider per image, so
# the two console postures cannot coexist in a single tree. Every other sim and QEMU
# gate runs kickos_services_none, where the kernel keeps the console, cap index 0 is
# unseated, and the endpoint route is never touched. This gate is the missing half:
# without it, a console handover that silences the whole test harness passes every
# other gate.
#
# The load-bearing assertion is the NEGATIVE one: the console driver's own kos::print
# banner must be ABSENT. It goes to the kernel debug console, which a published board
# drops by design, so its absence proves the handover really happened and the TAP
# stream we just read came through the endpoint, not through a silent fallback. Without
# it, a regression that skipped the publish entirely would still pass here.
#
# usage: check_sim_published.sh <kickos-source-dir> <cmake> <expected-arms> <variant>
#
# <variant> is not optional and is not cosmetic. The expected arm count is computed by
# the CALLING tree's CMake and depends on the posture, which is part of the variant the
# caller was configured with; the build below is a fresh one and would otherwise take
# the board's base variant. A caller on another variant would then compare its own
# expectation against a different posture's stream and fail with "an arm was added or
# deleted", naming a regression that does not exist. Forward the variant; do not infer
# the posture.

set -eu
. "$(dirname "$0")/lib/gate.sh"

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"
WANT_ARMS="${3:?usage: check_sim_published.sh <src> <cmake> <expected-arms> <variant>}"
VARIANT="${4:?usage: check_sim_published.sh <src> <cmake> <expected-arms> <variant>}"

scratch_dir

echo "== configuring the sim with the publishing service list =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim \
    -DKICKOS_CONFIG_VARIANT="$VARIANT" >/dev/null ) \
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

# The stream verdict itself is check_tap_stream.sh's, not a second copy of it here: plan
# vs case count vs expected arms, the completion marker, and the by-name EXPECT_SKIPS /
# EXPECT_PARTIALS permission sets, which a private parse would drift out of step with.
printf '%s\n' "$OUT" | "$(dirname "$0")/check_tap_stream.sh" sim_published "$WANT_ARMS"

echo "PASS: the full $WANT_ARMS-arm TAP stream is observable over a published userspace console"
