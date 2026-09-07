#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A task losing its last member while a peer core is inside its own death. Boot the
# `taskleave` image and assert the run SURVIVES and that no space destroy found a peer core
# still holding the space it was destroying.
#
# The survival half is the point on rv64: the kernel's own top-level entries live in the root
# page a destroy frees, so a core still walking that root crashes the kernel rather than
# merely losing a user mapping. A hang, a panic or a truncated run all fail here, the last two
# through the exit status: the image prints PASS from main and only then tears itself down.
#
# The image's own `space destroys` line is what keeps `release peer hits: 0` from being
# satisfied by a run in which no destroy ran; a shortfall there is its own FAIL line.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_taskleave.sh <taskleave.elf>}"

run_image "$elf"

if has "ERROR"; then
    fail "taskleave reported a failed setup"
fi
if has "TASKLEAVE FAIL"; then
    # TWO verdicts wear this marker, a peer hit and a destroy shortfall, so the image's own
    # line is what says which.
    printf '%s\n' "$OUT" | grep -F -e "TASKLEAVE FAIL"
    fail "the image reported a failed verdict"
fi
assert_no_panic "a member's death panicked the kernel"
if [ "$RC" -eq 124 ]; then
    fail "the image did not exit within ${QEMU_TIMEOUT:-20}s: a core walking freed tables?"
fi
require_on_wire "release peer hits: 0" \
    "the peer-hit counter did not read 0 (or the run never reached it)"
require_on_wire "TASKLEAVE PASS" "the run never reached PASS"
# PASS is printed from main and main then RETURNS: root's return, kickos_terminate, the
# console flush and arch_shutdown all run after the last line this gate can read, so the
# status is the only thing that covers them.
if [ "$RC" -ne 0 ]; then
    fail "the image printed PASS and then exited $RC: the teardown behind the verdict failed"
fi

echo "PASS: every space destroy ran with no core holding the space"
exit 0
