#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The translation-fault gate as the UNPRIVILEGED level sees it: run the `aspaceufault` image
# and assert that the process read a page the kernel had mapped into its space, that the same
# read killed the thread once the kernel unmapped it, at the page the kernel announced, with
# the syndrome a MISSING page gives rather than one a surviving leaf's permission would.
#
# The read must be the process's, and the control says it was: at the unprivileged level the
# read RETURNS while the leaf stands, where a kernel-side touch of a user page faults either way
# on a port that never lets the kernel reach one (rv64 never sets sstatus.SUM), and would
# satisfy every assertion below against a backend whose unmap did nothing.
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
#   check_aspace_ufault.sh <aspaceufault image> <dump-marker> <expect-status> <arch>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_ufault.sh <aspaceufault image> <dump-marker> <expect-status> <arch>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
arch="${4:?$_usage}"
require_number "$expect_status" "the expected exit status"

run_faulting_image "$elf"

answers="$(printf '%s\n' "$OUT" | grep -c '\[aspaceufault\] the mapping answers')"
if [ "$answers" -ne 1 ]; then
    printf '%s\n' "$OUT"
    fail "the process never read the page while it was mapped ($answers control markers)"
fi

announce="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p' \
    | tail -n 1)"
if [ -z "$announce" ]; then
    printf '%s\n' "$OUT"
    fail "the kernel never announced the page it unmapped, so no address comparison is possible"
fi

require_single_marker "$marker" "the unmapped page did not fault"

_where="the record faults somewhere other than 0x${announce}, the page the kernel unmapped"
case "$arch" in
    armv8a)
        field_matcher_control
        require_field_on_wire ADDR "0x${announce}" 'ADDR=|ESR_EL1=' "$_where"
        require_field_on_wire ESR_EL1 0x92000007 'ESR_EL1=' \
            "the syndrome is not a level-3 translation fault on a read from the lower level"
        ;;
    rv64imac)
        require_rv64_fault_at "$announce" "the page the kernel unmapped" \
            0xd "a load page fault" "$expect_status"
        ;;
    x86_64)
        field_matcher_control
        require_field_on_wire ADDR "0x${announce}" 'ADDR=|vec:err=' "$_where"
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

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${announce} answered while mapped, then faulted on $arch; exit $RC"
exit 0
