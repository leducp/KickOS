#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU gate for the full-C++ opt-in (Stage B of docs/design-kickcat-k64f.md): boot
# the cxxtest image on QEMU via semihosting and assert that exceptions, STL and
# RTTI all executed (every check printed PASS, and the "ALL PASS" summary). Proves
# the toolchain libstdc++/libsupc++ over newlib runs on the target ISA -- no HW.

set -u
elf="${1:?usage: check_qemu_cxxtest.sh <cxxtest.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"   # armv7m default; virt for RISC-V
extra_arg="${QEMU_EXTRA:-}"             # RISC-V virt needs -bios none

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-15}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

if echo "$out" | grep -qiE "panic|unreachable|FAIL:|SOME FAILED"; then
    echo "FAIL: a full-C++ check failed or the image faulted"
    exit 1
fi
if echo "$out" | grep -q "ALL PASS"; then
    echo "PASS: full-C++ exceptions/STL/RTTI executed under QEMU"
    exit 0
fi
echo "FAIL: 'ALL PASS' summary not observed (image did not complete)"
exit 1
