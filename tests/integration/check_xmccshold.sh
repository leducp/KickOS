#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of xmccshold (user/apps/xmc4800-relax/xmccshold): FEM=1 holds MSLS as one
# bracket (2 edges) and FEM=0 pulses it per word (6 to 10 edges), so the hardware CS-hold is
# usable, with no fault and no panic.
#
#   KOS_CAPTURE=<log> check_xmccshold.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture xmccshold

if has_e '^\[xmccshold\] (ERROR|FAIL)|: FAIL'; then
    jfail error "xmccshold reported a failure"
fi
if has_f '=== THREAD FAULT ==='; then
    jfail fault "a thread of xmccshold was killed"
fi
jno_panic "a panic in xmccshold"
jafter fem1-run 1 '[xmccshold] run 1 FEM=1: driving 4-word software-paced frame' "FEM=1 run"
jafter fem1-verdict "$AT" '[xmccshold] FEM=1: MSLS edges = 2 : PASS' "FEM=1 verdict"
jafter fem0-run "$AT" '[xmccshold] run 2 FEM=0: driving 4-word software-paced frame' "FEM=0 run"
jafter verdict "$AT" '[xmccshold] VERDICT: hardware CS-hold USABLE (FEM=1 holds, FEM=0 pulses)' "verdict"
echo "PASS: xmccshold showed FEM governs the SSC chip-select hold"
