#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the FRAMED arm of a published console endpoint. Runs simconabi, whose
# composition names the packaged simcon as stdout (a userspace console driver owns the wire),
# and requires that driver to ANSWER every kos_call it is sent.
#
# The defect is not a wrong answer, it is NO answer: a driver that does not separate the
# two protocols leaves the caller blocked forever. A hang is indistinguishable from slow,
# so this test's TIMEOUT is part of the assertion.
#
# The sim driver answers on its own code, not on uart_service.h's serve_one, so this is its
# only coverage; serve_one is the selftest's.
#
# usage: check_sim_conabi.sh <simconabi>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

APP="${1:?usage: check_sim_conabi.sh <simconabi>}"
[ -x "$APP" ] || fail "no simconabi image at $APP"

echo "== running simconabi against the published console =="
set +e
OUT="$("$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

# Anti-vacuity, FIRST: without a handover cap 0 is the kernel console and every call below
# fails for the wrong reason.
has '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"

[ "$RC" -eq 0 ] || fail "simconabi exited $RC"

# The stream verdict is check_tap_stream.sh's, so plan against case count, the completion
# marker and the skip/partial permission sets stay in one place.
printf '%s\n' "$OUT" | "$(dirname "$0")/check_tap_stream.sh" sim_console_abi 9

# The framed WRITE's payload reaches the wire as BYTES. Every line above is a verdict the
# app reached; this one is the route carrying data.
has '^\[conabi\] framed payload on the wire' \
  || fail "the framed write was acknowledged but its payload never reached the wire"

echo "PASS: a published console driver answers every framed op on its endpoint, and a request frame is never written to the wire as text"
