#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The stress app's own conservation verdict (user/apps/common/stress): STRESS PASS, and neither
# a FAIL nor a SKIP on a board whose pools were too small to run it. The sim runs the image; with
# KOS_CAPTURE, a silicon capture is read instead.
#
#   check_stress.sh <stress image>
#   KOS_CAPTURE=<log> check_stress.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${SIM_TIMEOUT:=50}"

if judging_capture; then
    judge_capture stress
else
    run_image "${1:?usage: check_stress.sh <stress image>}"
fi

assert_no_panic "stress panicked"
if ! has "^stress: scheduler"; then
    cfail start "the stress app never announced itself"
fi
if has "STRESS SKIP"; then
    cfail skip "stress skipped: the board's thread or semaphore pool is too small to run it"
fi
if has "STRESS FAIL"; then
    printf '%s\n' "$OUT" | grep -E "naps|STRESS FAIL"
    cfail error "stress counted a conservation failure"
fi
if ! has "^STRESS PASS"; then
    cfail verdict "stress reached no PASS"
fi
echo "PASS: stress conserved every nap, handoff and churn"
exit 0
