#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ESP32-C6 HP/LP AMP partition's silicon capture, judged by check_c6_amp_capture.py, in the
# shape tools/bench/bench.sh runs a JUDGE:
#
#   KOS_CAPTURE=<log> check_c6_amp_gate.sh <kickos-build> <kickos-source> <cmake>
#
# The whole log is judged, not its last boot: the ROM's load lines precede both banners.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_c6_amp_gate.sh <kickos-build> <kickos-source> <cmake>"
[ -n "${KOS_CAPTURE:-}" ] || fail "no KOS_CAPTURE: $USAGE"
[ -s "$KOS_CAPTURE" ] || fail "no capture at $KOS_CAPTURE"
python3 "$(dirname "$0")/check_c6_amp_capture.py" "$KOS_CAPTURE"
