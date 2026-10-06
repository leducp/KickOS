#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of c6blink (user/apps/esp32c6-wroom/c6blink): kos_periph_enable reaching the
# chip for the window's holder and refused to a thread holding none, GPIO10's pad tracking every
# drive, then the windowless thread killed for its write to GPIO10's matrix out-sel.
#
#   KOS_CAPTURE=<log> check_c6blink.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture c6blink

if has_e '^\[c6blink\] (ERROR|FAIL|.*DID NOT FAULT)'; then
    jfail error "c6blink reported a failure"
fi
jafter holder 1 '[c6blink] PASS periph_enable holder rc -38 (want -38)' "holder's periph_enable"
jafter blink-start "$AT" '[c6blink] blinking GPIO10 via the 64 B pin bank' "blink start"
jafter blink "$AT" '[c6blink] PASS (pad tracked the drive on every cycle)' "blink verdict"
jafter non-holder "$AT" '[c6blink] PASS periph_enable non-holder rc -1 (want -1)' "non-holder's refusal"
jafter announce "$AT" '[c6blink] poking UNGRANTED out-sel @ 0x6009157c (expect MPU FAULT)' "announce of the write"
require_killed_at c6poke 0x6009157c
echo "PASS: c6blink drove GPIO10 through its window and its windowless thread was killed for the out-sel write"
