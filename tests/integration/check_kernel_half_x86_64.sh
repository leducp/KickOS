#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel-half gate on x86_64: run the `kernelhalf` image and assert that an unprivileged
# thread reading a word of kernel writable state was killed, at the address the image named,
# with the error code a PROTECTED page gives and not the one a missing page would.
#
# The kernel's half is the firmware's identity map, present and supervisor-only in every
# space, so the read must fault present: error code 0x5 is P set (a protection fault on a
# present page), W clear (a read) and U/S set (from ring 3), vector 14 (Intel SDM Vol 3,
# 4.7). A code of 0x4 would say the page is absent from the task's space, which is a lost
# kernel half rather than a refused one. The thread-kill record prints the pair as
# vec:err=0x<vector><error, 8 digits>. Spelled out here rather than derived, so the gate
# asserts the encoding instead of restating whatever the source happens to produce.
set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_kernel_half_x86_64.sh <kernelhalf.efi> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
require_number "$expect_status" "the expected exit status"

run_faulting_image "$elf"

addr="$(printf '%s\n' "$OUT" | sed -n 's/.*\[kernelhalf\] reading 0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ -z "$addr" ]; then
    printf '%s\n' "$OUT"
    fail "the image named no address to read"
fi

require_single_marker "$marker" "reading the kernel's half did not fault"
field_matcher_control
require_field_on_wire ADDR "0x${addr}" 'ADDR=|vec:err=' \
    "the record faults somewhere other than 0x${addr}, the word the image named"
# The pair's name carries a colon, which the field matcher does not model, so its value is
# read off the record and compared whole.
got="$(printf '%s\n' "$OUT" | sed -n 's/.*vec:err=\(0x[0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ "$got" != "0xe00000005" ]; then
    printf '%s\n' "$OUT" | grep 'vec:err=' || :
    fail "the fault is not a protection fault on a present page, read from ring 3: vec:err=${got:-none}, expected 0xe00000005"
fi
if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${addr} in the kernel's half refused a ring-3 read (protection fault); exit $RC"
