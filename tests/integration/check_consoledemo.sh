#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of consoledemo (user/apps/xmc4800-relax/consoledemo): the packaged
# xmcuartirq's up line and the constructor's line, in either order, then main's and the worker's
# lines through the driver, in that order, with no panic.
#
#   KOS_CAPTURE=<log> check_consoledemo.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture consoledemo

if has_e '^\[consoledemo\] ERROR'; then
    jfail error "consoledemo reported an error"
fi
jno_panic "a panic in consoledemo"
jafter driver-up 1 '[xmcuartirq] device up (IRQ TX)' "xmcuartirq's up line"
_from="$AT"
jafter ctor-line 1 '[init] pre-publish ctor line' "constructor line"
if [ "$AT" -gt "$_from" ]; then
    _from="$AT"
fi
jafter main-line "$_from" '[main] post-publish line via the userspace driver' "main's line through the driver"
for _i in 0 1 2 3 4; do
    jafter worker-line "$AT" "[worker] line $_i via the userspace console driver" "worker line $_i"
done
jafter worker-done "$AT" '[worker] done' "worker's last line"
echo "PASS: consoledemo printed through xmcuartirq once it was up, in order"
