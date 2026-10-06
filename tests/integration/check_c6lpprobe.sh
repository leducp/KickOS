#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of c6lpprobe (user/apps/esp32c6-wroom/c6lpprobe): the LP core started from one
# flash, its payload and APM records, and the PMU doorbells rung both ways, with no timeout.
#
#   KOS_CAPTURE=<log> check_c6lpprobe.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture c6lpprobe

if has '^c6lpprobe: timeout'; then
    jfail timeout "the LP core never wrote its marker"
fi
jno_panic "a panic in c6lpprobe"
jafter payload 1 'c6lpprobe: payload IRQ ' "LP payload record" part
jafter doorbells "$AT" 'c6lpprobe: PASS bidirectional PMU doorbells' "doorbell verdict"
echo "PASS: c6lpprobe's LP core started and rang the PMU doorbells both ways"
