#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of inprstorm (user/apps/xmc4800-relax/inprstorm): the storm thread reroutes
# USIC0 channel 1's receive interrupt onto the console node and storms it, while the entry
# heartbeats. The console surviving is the point, so the capture must carry the reroute and two
# heartbeats after it, with no panic.
#
#   KOS_CAPTURE=<log> check_inprstorm.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture inprstorm

if has_f '[inprstorm] CCFG.TB=0'; then
    jfail no-fifo "the TX FIFO was absent, so the fifo profile could not run"
fi
jno_panic "a panic in inprstorm"
jafter root-up 1 '[inprstorm] MARKER: root up, spawning the U0C1 holder' "startup marker"
jafter reroute "$AT" '[inprstorm] rerouting INPR RINP/AINP -> SR0 (console node)' "reroute"
jafter heartbeat-1 "$((AT + 1))" '[inprstorm] heartbeat ' "a heartbeat after the reroute" part
jafter heartbeat-2 "$((AT + 1))" '[inprstorm] heartbeat ' "a second heartbeat after the reroute" part
echo "PASS: inprstorm rerouted the storm onto the console node and the heartbeat kept beating"
