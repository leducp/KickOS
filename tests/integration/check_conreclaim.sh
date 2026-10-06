#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of conreclaim (user/apps/xmc4800-relax/conreclaim): the test-owned testusic's
# up line, main's line through it, the driver's line before it scrambles the channel, then the
# panic verdict, which only the kernel reclaiming the published console can carry. The reclaim
# may frame one spurious byte ahead of the panic banner, so the verdict is matched within its line.
#
#   KOS_CAPTURE=<log> check_conreclaim.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture conreclaim

if has_e '^\[(conreclaim|testusic)\] ERROR'; then
    jfail error "conreclaim or testusic reported an error"
fi
jafter driver-up 1 '[testusic] driver up (polled TX)' "testusic's up line"
jafter writer "$AT" '[conreclaim] writing through the test console driver' "main's line through testusic"
jafter scramble "$AT" '[testusic] scrambling the channel, its clock gated last' "testusic's scramble line"
jafter verdict "$AT" 'KERNEL PANIC: [conreclaim] PASS: the dead channel came back' "reclaimed panic verdict" part
echo "PASS: conreclaim's panic reached the wire off the channel testusic scrambled"
