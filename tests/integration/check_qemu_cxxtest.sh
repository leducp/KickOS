#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate for the full-C++ opt-in: boot the cxxtest image on QEMU via semihosting, or read a
# silicon capture of it, and assert that exceptions, STL and RTTI all executed (every check
# printed PASS, and the "ALL PASS" summary). Proves the toolchain libstdc++/libsupc++ over
# newlib runs on the target ISA.
#
#   check_qemu_cxxtest.sh <cxxtest.elf>
#   KOS_CAPTURE=<log> check_qemu_cxxtest.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

if judging_capture; then
    judge_capture cxxtest
else
    elf="${1:?usage: check_qemu_cxxtest.sh <cxxtest.elf>}"
    need_qemu_machine
    run_image "$elf"
fi

assert_no_panic "the image faulted during the full-C++ checks"
if has_e "FAIL:|SOME FAILED"; then
    cfail error "a full-C++ check failed"
fi
if ! has "ALL PASS"; then
    cfail verdict "'ALL PASS' summary not observed (image did not complete)"
fi

echo "PASS: full-C++ exceptions/STL/RTTI executed"
exit 0
