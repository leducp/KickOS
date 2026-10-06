#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Judges an esp32c6-wroom-amp2 capture of the ampping partition (docs/design-m10-fleet.md,
# section 9): both nodes' images loaded by the ROM, the LP core's gate witness and the four rounds
# across, read by check_c6_amp_capture.py.
#
#   KOS_CAPTURE=<log> check_c6_amp_capture.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_c6_amp_capture.sh <board-build> <kickos-source> <cmake>"
: "${1:?$USAGE}" "${2:?$USAGE}" "${3:?$USAGE}"
if [ -z "${KOS_CAPTURE:-}" ]; then
    jfail no-capture "no KOS_CAPTURE: this partition is judged from a silicon capture only"
fi
[ -s "$KOS_CAPTURE" ] || jfail no-capture "no capture at $KOS_CAPTURE"
python3 "$(dirname "$0")/check_c6_amp_capture.py" "$KOS_CAPTURE"
