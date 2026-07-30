#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sim TAP gate: run the host `selftest` binary and hand the stream to
# tests/check_tap_stream.sh, which owns the verdict.

set -u
bin="${1:?usage: check_sim_selftest.sh <selftest-binary>}"
here="$(cd "$(dirname "$0")" && pwd)"

# The exit status is not the verdict: a reported fault still exits 0 via the fault path.
# The timeout is only a hang backstop.
out="$(timeout "${SIM_TIMEOUT:-30}" "$bin" 2>&1)"
echo "$out"
printf '%s\n' "$out" | "$here/check_tap_stream.sh" sim
