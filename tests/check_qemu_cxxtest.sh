#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU gate for the full-C++ opt-in (Stage B of docs/design-kickcat-k64f.md): boot
# the cxxtest image on QEMU via semihosting and assert that exceptions, STL and
# RTTI all executed (every check printed PASS, and the "ALL PASS" summary). Proves
# the toolchain libstdc++/libsupc++ over newlib runs on the target ISA -- no HW.

set -u
. "$(dirname "$0")/lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

elf="${1:?usage: check_qemu_cxxtest.sh <cxxtest.elf>}"

need_qemu_machine
run_image "$elf"

assert_no_panic "the image faulted during the full-C++ checks"
if has_e "FAIL:|SOME FAILED"; then
    fail "a full-C++ check failed"
fi
if ! has "ALL PASS"; then
    fail "'ALL PASS' summary not observed (image did not complete)"
fi

echo "PASS: full-C++ exceptions/STL/RTTI executed under QEMU"
exit 0
