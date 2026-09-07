#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A slay whose victim is RUNNING on a peer core and makes no syscall. Boot the `slaypeer`
# image and assert kos_thread_slay answered 0 within its own bound.
#
# The bound is the image's, not this gate's: a kernel that never displaces the victim answers
# -KOS_ETIMEDOUT (-110) and prints FAIL, so the failure arrives as a verdict with a reason
# rather than as this script's patience running out.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_slaypeer.sh <slaypeer.elf>}"

run_image "$elf"

if has "ERROR"; then
    fail "slaypeer reported a failed setup"
fi
if has "SLAYPEER FAIL"; then
    fail "the slay did not reach its victim: a core re-picked its own slain current"
fi
assert_no_panic "the slay panicked the kernel"
if [ "$RC" -eq 124 ]; then
    fail "the image did not exit within ${QEMU_TIMEOUT:-20}s: the slay's own bound would have
  arrived as a FAIL line, so this is the run failing to terminate"
fi
require_on_wire "slay rc: 0" \
    "the slay answered something other than 0 (or the run never reached it)"
require_on_wire "SLAYPEER PASS" "the run never reached PASS"
# PASS is printed from main and main then RETURNS: root's return, kickos_terminate, the
# console flush and arch_shutdown all run after the last line this gate can read, so the
# status is the only thing that covers them.
if [ "$RC" -ne 0 ]; then
    fail "the image printed PASS and then exited $RC: the teardown behind the verdict failed"
fi

echo "PASS: a victim running on a peer core lost that core and ran its own teardown"
exit 0
