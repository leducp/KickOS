#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The user-fault gate on x86_64: run the `aspaceufault` image, which reads a page of its own
# space, has the kernel unmap it and reads it again, and assert that the second read killed the
# thread at the page the kernel announced, with the error code a MISSING page gives.
#
# Error code 0x4 is P clear (the page is not present), W clear (a read) and U/S set (from
# ring 3), vector 14 (Intel SDM Vol 3, 4.7). A 0x5 would mean the leaf survived the unmap and
# only its permission refused, so the gate separates the two. The thread-kill record prints the
# pair as vec:err=0x<vector><error, 8 digits>.
set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_ufault_x86_64.sh <aspaceufault.efi> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
require_number "$expect_status" "the expected exit status"

run_faulting_image "$elf"

page="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p' | tail -n 1)"
if [ -z "$page" ]; then
    printf '%s\n' "$OUT"
    fail "the kernel never announced the page it unmapped"
fi

require_single_marker "$marker" "reading the unmapped page did not fault"
field_matcher_control
require_field_on_wire ADDR "0x${page}" 'ADDR=|vec:err=' \
    "the record faults somewhere other than 0x${page}, the page the kernel unmapped"
# The pair's name carries a colon, which the field matcher does not model, so its value is
# read off the record and compared whole.
got="$(printf '%s\n' "$OUT" | sed -n 's/.*vec:err=\(0x[0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ "$got" != "0xe00000004" ]; then
    printf '%s\n' "$OUT" | grep 'vec:err=' || :
    fail "the fault is not a ring-3 read of a page that is not present: vec:err=${got:-none}, expected 0xe00000004"
fi
if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${page} faulted not-present on a ring-3 read after the unmap; exit $RC"
