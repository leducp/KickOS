#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the thread_local witness under QEMU and require its own verdict.
#
# The image decides PASS/FAIL itself, because the properties it checks are ones only it can
# see: that each thread reads back what IT wrote, that the storage is at a DIFFERENT address
# per thread, and that the .tdata template reached a freshly spawned thread. A process-wide
# fallback passes the first of those and fails the second, which is the silent failure the
# whole mechanism exists to prevent.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_tlsprobe.sh <tlsprobe.elf>}"

poll_image "$elf" "\[tlsprobe\] (PASS|FAIL)"

if [ "$POLL_OK" -ne 1 ]; then
    fail "tlsprobe reached no verdict: neither PASS nor FAIL was printed"
fi
if has "\[tlsprobe\] FAIL"; then
    fail "tlsprobe reported FAIL"
fi
assert_no_panic "tlsprobe panicked before reaching a verdict"

echo "PASS: QEMU tlsprobe gave every thread its own thread_local"
exit 0
