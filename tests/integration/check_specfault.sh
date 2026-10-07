#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# specfault (user/apps/common/specfault): the read of the probe address faulted cleanly. On the
# Teensy that address is the unbacked FlexSPI wrap, and a wrap that regressed to readable stalls
# the core instead, so a capture ending at the announcement is the regression; elsewhere it is
# an address nothing grants the thread. Built only where the image enforces memory
# (user/apps/common/CMakeLists.txt). Boots the image on QEMU when QEMU_MACHINE is set, natively
# otherwise, and reads a silicon capture with KOS_CAPTURE.
#
#   check_specfault.sh <specfault image>
#   KOS_CAPTURE=<log> check_specfault.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if judging_capture; then
    judge_capture specfault
else
    run_image "${1:?usage: check_specfault.sh <specfault image>}"
fi
want="$(printf '%s\n' "$OUT" | sed -n 's/^\[specfault\] reading 0x\([0-9a-fA-F]*\):.*/\1/p' | head -n1)"
if [ -z "$want" ]; then
    cfail announce "specfault never announced the address it reads"
fi
if has '^\[specfault\] ERROR'; then
    cfail error "the read of the probe address was permitted"
fi
_said="$(printf '%s\n' "$OUT" | grep -n '^\[specfault\] reading 0x' | head -n1 | cut -d: -f1)"
_fault="$(printf '%s\n' "$OUT" | grep -nE "^=== MPU FAULT|^MPU FAULT: thread|^$(thread_fault_re main)" \
    | head -n1 | cut -d: -f1)"
if [ -z "$_fault" ]; then
    cfail fault "no fault report: the read of the probe address was not denied"
fi
if [ "$_fault" -le "$_said" ]; then
    cfail order "the fault report at line $_fault precedes the app's read at line $_said"
fi
_addr="$(reported_fault_addr)"
if [ -z "$_addr" ]; then
    cfail address "the fault report carries no address"
fi
if [ "$((0x$_addr))" -ne "$((0x$want))" ]; then
    cfail address "the fault was at 0x$_addr, not at the probe address 0x$want"
fi
echo "PASS: the read of the probe address faulted cleanly at 0x$want"
exit 0
