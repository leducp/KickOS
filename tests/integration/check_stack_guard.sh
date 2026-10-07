#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The stack guard-page gate: run the `stackguard` image and assert that an unprivileged
# thread walking DOWN off its own stack faulted, on the page the image last named, with the
# syndrome an unmapped page gives and not the one a permission would.
#
# Four assertions, and the first is the one that separates a guard page from a short stack.
# The image announces every probe BEFORE making it, so the number of announcements says how
# many pages of the stack were written successfully: at least two means a page inside the
# stack really was reachable and a lower one was not, which is the property. The record's
# ADDR must equal the LAST announced address, so a fault anywhere else fails rather than
# counting. The syndrome must name a WRITE to a page carrying no entry, taken from the
# unprivileged level: a permission fault would mean the neighbour was mapped and merely
# read-only. And the image must print no ERROR line, so a write that quietly succeeded past
# the whole stack cannot pass as a fault.
#
# THE SYNDROME IS READ OFF THE THREAD-KILL RECORD AND NOT A PANIC DUMP: the walk is an
# unprivileged access, so the backend contains it, and the image dies of it and the system
# does not. Each encoding is spelled out here rather than derived, so this gate asserts the
# encoding instead of restating whatever the source happens to produce:
#   armv8a    ESR_EL1 0x92000047 is EC 0x24 (data abort, lower exception level), IL 1 (a
#             32-bit instruction), WnR set (a write) and DFSC 0b000111 (translation fault,
#             level 3) (DDI 0487 M.b, ESR_EL1).
#   rv64imac  scause 15 is a store page fault where 13 is a load, and no field beside it names
#             a level or tells a missing leaf from a refusing one (RISC-V Privileged ISA,
#             Supervisor Cause Register).
#   x86_64    vector 0xe with error code 6: P clear (not present), W/R set (a write), U/S set
#             (ring 3) (SDM Vol. 3A, Page-Fault Error Code).
#
# ABOVE ONE CORE THE WIRE HAS NO PER-LINE ATOMICITY, arch_console_write being a byte-at-a-time
# device loop under no lock, so a core printing a status line lands it INSIDE another's line
# character by character. The MARKER is a presence, so it is matched byte-exact first and a
# split is tolerated second, through require_on_wire, which reports the span it allowed.
#
# THE PROBE COUNT AND THE FIELD READS STAY BYTE-EXACT, and that is not a gap: a bounded
# in-order match answers whether a literal reached the wire and never how many times nor which
# value it carried, so it can neither count the probes nor separate the page last named from a
# longer address. Each refusal says which of the two readings applies where it can tell.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_stack_guard.sh <stackguard image> <dump-marker> <expect-status> <arch>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
arch="${4:?$_usage}"
require_number "$expect_status" "the expected exit status"

# An absence, so weak by nature on a wire with more than one writer. Corroborated: a write that
# ran off the stack unpunished leaves no marker, no ADDR at the page last named and no fault
# status, and all three are asserted positively below.
run_faulting_image "$elf"

probes="$(printf '%s\n' "$OUT" | sed -n 's/.*\[stackguard\] touching 0x\([0-9a-f]*\).*/\1/p')"
count="$(printf '%s\n' "$probes" | grep -c '[0-9a-f]')"
if [ "$count" -lt 2 ]; then
    printf '%s\n' "$OUT"
    _split=""
    if [ "$(wire_cores)" -gt 1 ]; then
        _split=", or a probe line a peer's status line broke into, which leaves how many pages
  were written UNREADABLE rather than short: this read is a COUNT, and a bounded in-order match
  answers whether a literal reached the wire and never how many times"
    fi
    fail "only $count page(s) probed, so the first probe faulted and no page of the stack was
  ever written$_split"
fi
last="$(printf '%s\n' "$probes" | tail -n 1)"

require_single_marker "$marker" "walking below the stack did not fault"

_where="the record faults somewhere other than 0x${last}, the page the image last named"
case "$arch" in
    armv8a)
        field_matcher_control
        require_field_on_wire ADDR "0x${last}" 'ADDR=|ESR_EL1=' "$_where"
        require_field_on_wire ESR_EL1 0x92000047 'ESR_EL1=' \
            "the syndrome is not a level-3 translation fault on a write from the lower level"
        ;;
    rv64imac)
        require_rv64_fault_at "$last" "the page the image last named" \
            0xf "a store page fault" "$expect_status"
        ;;
    x86_64)
        field_matcher_control
        require_field_on_wire ADDR "0x${last}" 'ADDR=|vec:err=' "$_where"
        got="$(printf '%s\n' "$OUT" | sed -n 's/.*vec:err=\(0x[0-9a-f]*\).*/\1/p' | tail -n 1)"
        if [ "$got" != "0xe00000006" ]; then
            printf '%s\n' "$OUT" | grep 'vec:err=' || :
            fail "the fault is not a ring-3 write to a page that is not present:
  vec:err=${got:-none}, expected 0xe00000006"
        fi
        ;;
    *)
        fail "no syndrome is spelled here for $arch"
        ;;
esac

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: $count page(s) of the stack written, then 0x${last} faulted on $arch; exit $RC"
exit 0
