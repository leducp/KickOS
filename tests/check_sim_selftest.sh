#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sim TAP gate: run the host `selftest` binary and hand the stream to
# tests/check_tap_stream.sh, which owns the verdict.

set -u
here="$(dirname "$0")"
. "$here/lib/gate.sh"
: "${SIM_TIMEOUT:=30}"

bin="${1:?usage: check_sim_selftest.sh <selftest-binary> <expected-arms>}"
want_arms="${2:?usage: check_sim_selftest.sh <selftest-binary> <expected-arms>}"

# The exit status is not the verdict: a reported fault still exits 0 via the fault path.
run_image "$bin"
printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" sim "$want_arms"
