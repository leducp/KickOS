#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of gpioblink (user/apps/common/gpioblink): the task muxed its LED pin and
# took its port window, and the pad read back every one of its ten drives, high then low.
#
#   KOS_CAPTURE=<log> check_gpioblink.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture gpioblink
jno_panic "a panic on gpioblink's path"
if has '^=== THREAD FAULT ==='; then
    jfail fault "a thread faulted"
fi
if ! has '^\[gpioblink\] driving port '; then
    jfail start "gpioblink never announced its pin"
fi
if has '^\[gpioblink\] ERROR'; then
    jfail error "gpioblink could not mux its pin or take its window"
fi
_cycles="$(printf '%s\n' "$OUT" | grep -c '^\[gpioblink\] cycle [0-9]* led=1 readback=1 / led=0 readback=0')"
if [ "$_cycles" -ne 10 ]; then
    jfail cycle "$_cycles of the 10 cycles read back their drive"
fi
if has '^\[gpioblink\] FAIL'; then
    jfail verdict "gpioblink reported FAIL"
fi
if ! has '^\[gpioblink\] PASS (10 cycles, readback ok)'; then
    jfail verdict "gpioblink reached no PASS"
fi
echo "PASS: the pad tracked all 10 drives through the task's own window"
exit 0
