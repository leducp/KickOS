#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of k64drv (user/apps/frdmk64f/k64drv): LPTMR0 counting the LPO, its ten ticks
# in order on the line, then a thread holding no window reading the timer through the open slot,
# with no fault and no panic.
#
#   KOS_CAPTURE=<log> check_k64drv.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture k64drv

if has_e '^\[k64drv\] ERROR'; then
    jfail error "k64drv reported an error"
fi
if has_f '=== THREAD FAULT ==='; then
    jfail fault "a thread was killed, the windowless reader's slot read included"
fi
jno_panic "a panic in k64drv"
jafter timer-start 1 '[k64drv] LPTMR0 counting the 1 kHz LPO, compare every 250 ms' "timer start"
_from="$AT"
_n=1
while [ "$_n" -le 10 ]; do
    _at="$(printf '%s\n' "$OUT" | awk -v from="$_from" -v want="[k64drv] tick $_n" \
        'NR > from && $0 == want { print NR; exit }')"
    [ -n "$_at" ] || jfail tick "no tick $_n after line $_from of the capture"
    _from="$_at"
    _n=$((_n + 1))
done
jafter slot-read "$_from" '[k64drv] a thread holding no window read CNR=' "windowless slot read" part
jafter done "$AT" '[k64drv] done' "end of the run"
echo "PASS: k64drv took ten LPTMR0 ticks on its line and a windowless thread read the open slot"
