#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of rxirq (user/apps/rx72m/rxirq): IRQ15, a plain INTB vector, claimed on P07
# and driven from the pin's own latch, wakes its waiter once per low drive and not while the pin
# is high, and the sleep taken with the level request standing ends, so the line was masked on
# fire and the switcher still ran.
#
#   KOS_CAPTURE=<log> check_rxirq.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture rxirq

if has_e '^\[rxirq\] (ERROR|FAIL)'; then
    jfail error "rxirq reported a failure"
fi
jno_panic "something panicked"
jafter mux 1 '[rxirq] pinmux P07 -> IRQ15 input rc 0 (want 0)' "successful mux of P07"
jafter claim "$AT" '[rxirq] claim line 79 rc 0 (want 0)' "claim of IRQ15's vector"
_n=1
while [ "$_n" -le 5 ]; do
    _rel="$(printf '%s\n' "$OUT" | sed -n "$AT,\$p" | grep -n -m1 "^\[rxirq\] cycle $_n: " | cut -d: -f1)"
    [ -n "$_rel" ] || jfail cycle "no cycle $_n after line $AT: a standing request that is never
    masked keeps every thread out"
    AT=$((AT + _rel - 1))
    _line="$(printf '%s\n' "$OUT" | sed -n "${AT}p")"
    _f="$(printf '%s\n' "$_line" | sed -n 's/^\[rxirq\] cycle [0-9]*: pad \([0-9]\)\/\([0-9]\), low woke rc \(-*[0-9]*\) bits \(0x[0-9a-f]*\), high rc \(-*[0-9]*\)$/\1 \2 \3 \4 \5/p')"
    [ -n "$_f" ] || jfail cycle "cycle $_n is not in the shape the app prints: $_line"
    set -- $_f
    if [ "$1" != 0 ] || [ "$2" != 1 ]; then
        jfail pad "cycle $_n: the pad read $1/$2, not the 0/1 the latch drove"
    fi
    if [ "$3" != 0 ] || [ "$4" != 0x1 ]; then
        jfail wake "cycle $_n: the low drive woke nothing (rc $3, bits $4)"
    fi
    if [ "$5" != -110 ]; then
        jfail quiet "cycle $_n: the wait while the pin was high answered $5, not -KOS_ETIMEDOUT"
    fi
    _n=$((_n + 1))
done
jafter pass "$AT" '[rxirq] PASS (one wake per low drive, none while high)' "pass verdict after cycle 5"
echo "PASS: IRQ15 woke its waiter once per low drive on P07, masked on fire, and the clock ran"
