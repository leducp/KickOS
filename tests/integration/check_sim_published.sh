#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the POST-PUBLISH console posture: run the selftest image whose composition names
# the packaged simcon as `stdout` (user/apps/common/selftest/consoles/sim/simcon.yaml; a userspace
# console driver owns the "wire", see system/driver/sim/simcon/simcon.cc) and require the TAP
# stream to arrive over the DRIVER, clean. Every other sim and QEMU selftest gate runs under
# `stdout: kernel`, where cap index 0 is unseated and the endpoint route is never touched, so a
# handover that silences the whole test harness passes all of them.
#
# The load-bearing assertion is the NEGATIVE one: the console driver's own raw kernel console
# banner must be ABSENT. It goes to the kernel debug console, which a published board
# drops by design, so its absence proves the handover really happened and the TAP
# stream we just read came through the endpoint, not through a silent fallback. Without
# it, a regression that skipped the publish entirely would still pass here.
#
# usage: [EXPECT_SKIPS=...] [EXPECT_PARTIALS=...] [EXPECT_FAULTS=...] \
#        check_sim_published.sh <selftest image> <expected-arms>
# The EXPECT_* sets are read from the environment by check_tap_stream.sh.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

APP="${1:?usage: check_sim_published.sh <selftest image> <expected-arms>}"
WANT_ARMS="${2:?usage: check_sim_published.sh <selftest image> <expected-arms>}"
[ -x "$APP" ] || fail "no selftest image at $APP"

echo "== running selftest against the published console =="
set +e
OUT="$("$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

[ "$RC" -eq 0 ] || fail "selftest exited $RC (a failing test, or a truncated run)"

has '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"
if has 'kernel console diagnostic'; then
    fail "the kernel debug console is STILL live: no real handover, so this gate proved nothing"
fi
has_f "# tap route: stdout endpoint -> console driver (the composition's stdout)" \
  || fail "TAP did not take the published endpoint route"

# The stream verdict is check_tap_stream.sh's, so plan against case count against expected
# arms, the completion marker and the by-name permission sets stay in one place.
printf '%s\n' "$OUT" | "$(dirname "$0")/check_tap_stream.sh" sim_published "$WANT_ARMS"

echo "PASS: the full $WANT_ARMS-arm TAP stream is observable over a published userspace console"
