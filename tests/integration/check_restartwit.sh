#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot restartwit under QEMU, or read a silicon capture of it. Its witness, image 1, is
# restarted once and reads an initialised and a zeroed static at each start, after its last
# start wrote both: two pristine starts, then main's report that the witness ended twice.
#
#   check_restartwit.sh <restartwit.elf>
#   KOS_CAPTURE=<log> check_restartwit.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if judging_capture; then
    judge_capture restartwit
else
    elf="${1:?usage: check_restartwit.sh <restartwit.elf>}"
    run_image "$elf"
fi

assert_no_panic "restartwit panicked"
if has_e "\[restartwit\] (FAIL|ERROR)"; then
    printf '%s\n' "$OUT" | grep "\[restartwit\] \(FAIL\|ERROR\)"
    cfail pristine "a start of the witness read data its image does not hold, or main failed"
fi
STARTS="$(printf '%s\n' "$OUT" | grep -c "^\[restartwit\] pristine start")"
if [ "$STARTS" -ne 2 ]; then
    cfail starts "the witness started pristine $STARTS time(s), not twice"
fi
if ! has "^\[restartwit\] the witness ended twice"; then
    cfail ended "main never reported the witness's two ends"
fi
echo "PASS: a restart of image 1 started from its image's data"
exit 0
