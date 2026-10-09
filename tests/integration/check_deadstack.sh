#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A page withdrawn under a reader that holds its translation. Boot the `deadstack` image and
# assert the kernel refused the dead page as a buffer, then the reader's read faulted at that
# page with the syndrome a MISSING page gives, and ended its task.
#
# Each encoding is spelled out here rather than derived, so this gate asserts the encoding
# instead of restating whatever the source happens to produce:
#   armv8a    ESR_EL1 0x92000007 is EC 0x24 (data abort, lower exception level), IL 1, WnR
#             clear (a read) and DFSC 0b000111 (translation fault, level 3) (DDI 0487 M.b).
#   rv64imac  scause 13 is a load page fault, the whole of what the arch publishes about the
#             access (RISC-V Privileged ISA, Supervisor Cause Register).
#   x86_64    vector 14 with error code 0x4: P clear (not present), W clear (a read), U/S set
#             (ring 3) (Intel SDM Vol 3, 4.7); 0x5 would be a leaf that survived the unmap. The
#             record prints the pair as vec:err=0x<vector><error, 8 digits>.
#
#   check_deadstack.sh <deadstack image> <dump-marker> <expect-status> <arch>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_deadstack.sh <deadstack image> <dump-marker> <expect-status> <arch>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
arch="${4:?$_usage}"
require_number "$expect_status" "the expected exit status"

need_qemu_machine
run_image "$elf"

if has "DEADSTACK FAIL"; then
    fail "the reader read the page after its unmap: its translation survived"
fi
if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi
page="$(printf '%s\n' "$OUT" \
    | sed -n 's/.*\[deadstack\] the reader on core [0-9]* holds 0x\([0-9a-f]*\).*/\1/p' \
    | tail -n 1)"
if [ -z "$page" ]; then
    printf '%s\n' "$OUT"
    if [ "$(wire_cores)" -gt 1 ] && wire_has "[deadstack] the reader on core"; then
        fail "the reader's line reached the wire across $WIRE_SPAN bytes with a peer's line broken
  into it, so the page it names is UNREADABLE rather than unannounced"
    fi
    fail "the reader never held the page, so its fault would prove nothing"
fi

require_on_wire "[deadstack] the kernel refused the dead page as a buffer" \
    "the kernel never refused the dead page as a buffer, so its user-access check went unasked"
require_single_marker "$marker" "the withdrawn page did not fault"

_where="the record faults somewhere other than 0x${page}, the page the victim's exit unmapped"
case "$arch" in
    armv8a)
        field_matcher_control
        require_field_on_wire ADDR "0x${page}" 'ADDR=|ESR_EL1=' "$_where"
        require_field_on_wire ESR_EL1 0x92000007 'ESR_EL1=' \
            "the syndrome is not a level-3 translation fault on a read from the lower level"
        ;;
    rv64imac)
        require_rv64_fault_at "$page" "the page the victim's exit unmapped" \
            0xd "a load page fault" "$expect_status"
        ;;
    x86_64)
        field_matcher_control
        require_field_on_wire ADDR "0x${page}" 'ADDR=|vec:err=' "$_where"
        got="$(printf '%s\n' "$OUT" | sed -n 's/.*vec:err=\(0x[0-9a-f]*\).*/\1/p' | tail -n 1)"
        if [ "$got" != "0xe00000004" ]; then
            printf '%s\n' "$OUT" | grep 'vec:err=' || :
            fail "the fault is not a ring-3 read of a page that is not present:
  vec:err=${got:-none}, expected 0xe00000004"
        fi
        ;;
    *)
        fail "no syndrome is spelled here for $arch"
        ;;
esac

assert_no_panic "the withdrawal panicked the kernel"
if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${page} was read, then faulted once the victim's exit unmapped it, on $arch; exit $RC"
exit 0
