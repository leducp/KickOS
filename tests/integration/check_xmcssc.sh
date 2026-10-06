#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of xmcssc (user/apps/xmc4800-relax/xmcssc): the client's SSC transfers through
# the bus, every case passing and the loopback verdict, with no panic. The same source runs over
# the packaged service (proxy) or the local engine (KICKOS_SPI_LOCAL_ENGINE); the capture is the
# same either way.
#
#   KOS_CAPTURE=<log> check_xmcssc.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture xmcssc

if has_e '^\[xmcssc\] ERROR|: FAIL'; then
    jfail error "xmcssc reported a failure"
fi
jno_panic "a panic in xmcssc"
jafter bus-open 1 '[xmcssc] bus open: PASS' "bus open"
for _case in 'device open' 'single-byte loopback' 'multi-byte loopback' 'zero-tx loopback' \
             'two-segment transaction (one CS bracket)' 'oversized transfer refused'; do
    jafter case "$AT" "[xmcssc] $_case: PASS" "case $_case"
done
jafter loopback "$AT" '[xmcssc] loopback PASS (the SSC bus echoes tx == rx)' "loopback verdict"
echo "PASS: xmcssc ran every SSC case through the bus"
