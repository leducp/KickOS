#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of consoledemo (user/apps/xmc4800-relax/consoledemo): the constructor's line
# through the kernel console, then the packaged xmcuartirq's up line, then main's and the worker's
# lines through it, in that order, with no panic.
#
#   KOS_CAPTURE=<log> check_consoledemo.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture consoledemo

if has_e '^\[consoledemo\] ERROR'; then
    jfail error "consoledemo reported an error"
fi
jno_panic "a panic in consoledemo"
jafter ctor-line 1 '[init] pre-publish ctor line' "constructor line"
jafter driver-up "$AT" '[xmcuartirq] device up (IRQ TX)' "xmcuartirq's up line"
jafter main-line "$AT" '[main] post-publish line via the userspace driver' "main's line through the driver"
for _i in 0 1 2 3 4; do
    jafter worker-line "$AT" "[worker] line $_i via the userspace console driver" "worker line $_i"
done
jafter worker-done "$AT" '[worker] done' "worker's last line"
echo "PASS: consoledemo printed through the kernel, then through xmcuartirq, in order"
