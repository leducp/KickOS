#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A page withdrawn while a reader on a peer core holds its translation. Boot the `unmappeer`
# image and assert the reader's next read faulted at that page and ended its task.
#
# This is the TLB maintenance of the unmap judged by its result: a maintenance operation that
# reaches only the core issuing it leaves the reader's core reading the frame, and the image
# says so (UNMAPPEER FAIL) instead of faulting.
#
#   check_unmappeer.sh <unmappeer image> <dump-marker> <expect-status>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_unmappeer.sh <unmappeer image> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
require_number "$expect_status" "the expected exit status"

need_qemu_machine
run_image "$elf"

if has "UNMAPPEER FAIL"; then
    fail "the peer core read the page after its unmap: its stale translation survived"
fi
if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi
page="$(printf '%s\n' "$OUT" | sed -n 's/.*\[unmappeer\] the peer core reads 0x\([0-9a-f]*\).*/\1/p' \
    | tail -n 1)"
if [ -z "$page" ]; then
    fail "the reader never held the page, so its fault would prove nothing"
fi
require_single_marker "$marker" "the withdrawn page did not fault on the peer core"
field_matcher_control
require_field_on_wire ADDR "0x${page}" 'ADDR=' \
    "the record faults somewhere other than 0x${page}, the page the victim's exit unmapped"
assert_no_panic "the withdrawal panicked the kernel"
if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: the peer core faulted on 0x${page} once its unmap landed; exit $RC"
exit 0
