#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An exception nothing catches: the terminate handler names the exception's type and what(),
# then stops the image. A capture carries no exit status, so only the emulator sees the stop.
#
#   check_qemu_cxxterm.sh <cxxterm.elf>
#   KOS_CAPTURE=<log> check_qemu_cxxterm.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

if judging_capture; then
    judge_capture cxxterm
else
    elf="${1:?usage: check_qemu_cxxterm.sh <cxxterm.elf>}"
    need_qemu_machine
    run_image "$elf"
fi

assert_no_panic "the image faulted on its uncaught exception"
has "^cxxterm: throwing" || cfail throw "the image never reached its throw"
if has "^cxxterm: returned"; then
    cfail returned "an uncaught exception returned to its thrower"
fi
printf '%s\n' "$OUT" | grep -qxF "terminate: uncaught exception [St13runtime_error]: cxxterm uncaught" \
    || cfail terminate "terminate did not report the exception's type and what()"
status_clause "terminate stopped the image" ended
echo "PASS: terminate reported the uncaught exception"
