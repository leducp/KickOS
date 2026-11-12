#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot `svcfp` under QEMU, or read a silicon capture of it, and require its own verdict: an
# FP-live caller's s0-s15 and FPSCR come back as it set them from a register call the SVC
# fastpath refused and from a plain syscall.
#
#   check_svcfp.sh <svcfp.elf>
#   KOS_CAPTURE=<log> check_svcfp.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if judging_capture; then
    judge_capture svcfp
    POLL_OK=0
    if has "^\[svcfp\] PASS"; then
        POLL_OK=1
    fi
else
    elf="${1:?usage: check_svcfp.sh <svcfp.elf>}"
    poll_image "$elf" "\[svcfp\] PASS"
fi

if has_e "\[svcfp\] (CORRUPTED|ERROR)"; then
    printf '%s\n' "$OUT" | grep -E "\[svcfp\] (CORRUPTED|ERROR)"
    cfail error "svcfp reported a failed arm"
fi
if [ "$POLL_OK" -ne 1 ]; then
    cfail verdict "svcfp reached no PASS"
fi
assert_no_panic "svcfp panicked before reaching a verdict"
echo "PASS: svcfp got its FP state back from both arms of the SVC slow path"
exit 0
