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
#
# THE POLL WAITS FOR THE PASS AND NOT FOR A VERDICT. On a four-core posture the console has no
# per-line atomicity, so a FAIL line can arrive with a peer's status line broken into it and no
# grep for it can see that: waiting for the PASS is what makes a shredded FAIL a bound that ran
# out, which is a red. The FAIL grep below is the diagnostic that names it when it survived.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_tlsprobe.sh <tlsprobe.elf>}"

poll_image "$elf" "\[tlsprobe\] PASS"

if has "\[tlsprobe\] FAIL"; then
    printf '%s\n' "$OUT" | grep "\[tlsprobe\] FAIL"
    fail "tlsprobe reported FAIL"
fi
if [ "$POLL_OK" -ne 1 ]; then
    fail "tlsprobe reached no PASS inside ${QEMU_TIMEOUT:-8}s: it printed no verdict, or the
  one it printed was not a PASS"
fi
assert_no_panic "tlsprobe panicked before reaching a verdict"

echo "PASS: QEMU tlsprobe gave every thread its own thread_local"
exit 0
