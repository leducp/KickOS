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
#
#   check_slaypeer.sh <slaypeer.elf>
#   KOS_CAPTURE=<log> check_slaypeer.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if judging_capture; then
    judge_capture slaypeer
else
    elf="${1:?usage: check_slaypeer.sh <slaypeer.elf>}"
    run_image "$elf"
fi

if has "ERROR"; then
    cfail setup "slaypeer reported a failed setup"
fi
if has "SLAYPEER FAIL"; then
    cfail kept "the slay did not reach its victim: a core re-picked its own slain current"
fi
assert_no_panic "the slay panicked the kernel"
require_on_wire "slay rc: 0" \
    "the slay answered something other than 0 (or the run never reached it)" rc
require_on_wire "SLAYPEER PASS" "the run never reached PASS" pass
# PASS is printed from main and main then RETURNS: its task's end, the init's shutdown, the
# console flush and arch_shutdown all run after the last line this gate can read, so the
# status is the only thing that covers them.
status_clause "the system exited 0 once main returned past PASS" 0
if judging_capture; then
    echo "PASS: a victim running on a peer core lost that core"
    exit 0
fi

echo "PASS: a victim running on a peer core lost that core and ran its own teardown"
exit 0
