#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of k64console (user/apps/frdmk64f/k64console): the packaged k64uartirq's up
# line and the constructor's line, in either order, then main's and the worker's lines through the
# driver, in that order. A build with K64CONSOLE_SCRAMBLE_TEST is judged instead by its panic
# verdict reaching the wire after the driver came up, reclaimed from the published console.
#
#   KOS_CAPTURE=<log> check_k64console.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_k64console.sh <board-build> <kickos-source> <cmake>"
BUILD="${1:?$USAGE}"
[ -f "$BUILD/CMakeCache.txt" ] || jfail no-cache "no $BUILD/CMakeCache.txt to read the build's mode from"
judge_capture k64console

if has_e '^\[k64console\] ERROR'; then
    jfail error "k64console reported an error"
fi
jafter driver-up 1 '[k64uartirq] device up (IRQ TX/RX)' "k64uartirq's up line"
_up="$AT"
jafter ctor-line 1 '[init] pre-publish ctor line' "constructor line"
_from="$_up"
if [ "$AT" -gt "$_from" ]; then
    _from="$AT"
fi
if grep -q '^K64CONSOLE_SCRAMBLE_TEST:BOOL=ON' "$BUILD/CMakeCache.txt"; then
    jafter verdict "$_up" 'KERNEL PANIC: [k64console] PASS: the dead channel came back' "reclaimed panic verdict"
    echo "PASS: k64console's panic reached the wire out of the published console"
    exit 0
fi
jno_panic "a panic in k64console"
jafter main-line "$_from" '[main] post-publish line via the userspace driver' "main's line through the driver"
for _i in 0 1 2 3 4; do
    jafter worker-line "$AT" "[worker] line $_i via the userspace console driver" "worker line $_i"
done
jafter worker-done "$AT" '[worker] done' "worker's last line"
echo "PASS: k64console printed through k64uartirq once it was up, in order"
