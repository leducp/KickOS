#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the per-thread errno witness under QEMU and require its own verdict. The app
# provokes two DIFFERENT errno values out of newlib itself and reads each back after a
# round trip that crossed the other thread's write; see its main.cc for what each arm
# rules out.
set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_errnoprobe.sh <errnoprobe.elf>}"

poll_image "$elf" "\[errnoprobe\] (PASS|FAIL)"

if [ "$POLL_OK" -ne 1 ]; then
    fail "errnoprobe reached no verdict: neither PASS nor FAIL was printed"
fi
if has "\[errnoprobe\] FAIL"; then
    fail "errnoprobe reported FAIL"
fi
assert_no_panic "errnoprobe panicked before reaching a verdict"

echo "PASS: QEMU errnoprobe gave every thread its own errno"
exit 0
