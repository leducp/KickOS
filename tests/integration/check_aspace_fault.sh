#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The address-space fault gate: run the `aspacefault` image and assert that the page the
# kernel unmapped UNDER THE RUNNING TRANSLATION faulted, and that it is the page the kernel
# said it would be.
#
# Three assertions, and the second is the one that separates a real fault from a load that
# quietly answered a value. The image prints an ERROR line and exits non-zero on every path
# where the read returned, so a silent zero cannot pass as a fault. The dump's faulting address
# must equal the address the kernel announced before it switched, so a fault somewhere else (a
# stray access on the way, or the wrong page unmapped) fails rather than counting. And the
# syndrome must be a TRANSLATION fault on a read taken at the kernel's own level rather than a
# permission fault or an alignment fault, which would mean the entry was still there.
#
# Each encoding is spelled out here rather than derived, so this gate asserts the encoding
# instead of restating whatever the source happens to produce:
#   armv8a    FAR, and ESR 0x96000007: EC 0x25 (data abort, no change in exception level), IL 1
#             (a 32-bit instruction) and DFSC 0b000111 (translation fault, level 3), with WnR
#             clear for a read (DDI 0487 M.b, ESR_EL1).
#   rv64imac  stval, and scause 13, a load page fault, which the S-trap report says it took
#             from supervisor mode (RISC-V Privileged ISA, Supervisor Cause Register).
#
# ABOVE ONE CORE THE WIRE HAS NO PER-LINE ATOMICITY, arch_console_write being a byte-at-a-time
# device loop under no lock, so a core printing a status line lands it INSIDE another's line
# character by character. The MARKER is a presence, so it is matched byte-exact first and a
# split is tolerated second, through require_on_wire, which reports the span it allowed.
#
# THE THREE READS THAT CARRY A VALUE STAY BYTE-EXACT, and that is not a gap: a bounded in-order
# match answers whether a literal reached the wire and never which value it carried, so one
# foreign digit inside a record would satisfy the announced page with a longer one. All three
# tell a split apart from an absence instead, because the two send a reader to different places.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_fault.sh <aspacefault.elf> <dump-marker> <expect-status> <arch>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"
arch="${4:?$_usage}"

need_qemu_machine
run_image "$elf"

# An absence, so weak by nature on a wire with more than one writer. Corroborated: a read that
# quietly returned leaves no marker, no FAR at the announced page and no fault status, and all
# three are asserted positively below.
if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi

announce="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p')"
if [ -z "$announce" ]; then
    if [ "$(wire_cores)" -gt 1 ] && wire_has "[aspace] unmapped 0x"; then
        fail "the kernel's unmapped-page line reached the wire across $WIRE_SPAN bytes with a
  peer's line broken into it, so the page it names is UNREADABLE rather than unannounced. The
  page a shuffled line was going to name cannot be recovered from it"
    fi
    fail "the kernel never announced the page it unmapped, so no FAR comparison is possible"
fi

require_single_marker "$marker" "the unmapped page did not fault"

_where="the dump faults somewhere other than 0x${announce}, the page the kernel unmapped"
field_matcher_control
case "$arch" in
    armv8a)
        require_field_on_wire FAR "0x${announce}" 'FAR=|ESR=' "$_where"
        require_field_on_wire ESR 0x96000007 'ESR=' \
            "the syndrome is not a level-3 translation fault on a read at the current level"
        ;;
    rv64imac)
        require_field_on_wire stval "0x${announce}" 'stval=|scause=' "$_where"
        require_field_on_wire scause 0xd 'scause=' "the cause is not a load page fault"
        require_on_wire "taken from supervisor-mode" "the fault was not the kernel's own read"
        ;;
    *)
        fail "no syndrome is spelled here for $arch"
        ;;
esac

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${announce} faulted on $arch and the system stopped with $RC"
exit 0
