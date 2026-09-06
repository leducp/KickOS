#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel-half revocation gate: run the `kernelhalf` image and assert that an
# unprivileged thread reading a word of kernel writable state faulted, at the address the
# image named, with the syndrome a REVOKED mapping gives and not the one a missing mapping
# would.
#
# Four assertions, and the third is the one that separates this from every other fault gate.
# The image must announce the address, so a run that could not obtain one fails instead of
# passing on a silent skip. The reported fault address must equal that announcement, so a
# fault raised somewhere else cannot stand in. The syndrome must be a PERMISSION fault and
# not a translation fault: the kernel's half is mapped, at the same address, for the
# privileged side of the same core, so an image reporting a translation fault has lost the
# kernel's own window rather than revoked EL0's. And no ERROR line may appear, so a read
# that quietly returned a value cannot pass as a refusal.
#
# THE SYNDROME IS READ OFF THE THREAD-KILL RECORD AND NOT A PANIC DUMP. The read is an EL0
# access, so armv8a now contains it: the same ESR and the same address arrive through
# kickos_fault_record as `ESR_EL1=` and `ADDR=` instead of the reporter's `ESR=` and `FAR=`.
# The image dies of it and the system does not.
#
# ESR 0x9200000e is EC 0x24 (data abort, lower exception level), IL 1 (a 32-bit
# instruction), WnR clear (a read) and DFSC 0b001110 (permission fault, level 2), which is
# the level of the 2 MiB block descriptor carrying the permission (DDI 0487 M.b, ESR_EL1).
# Spelled out here rather than derived, so this gate asserts the encoding instead of
# restating whatever the source happens to produce.
#
# ABOVE ONE CORE THE WIRE HAS NO PER-LINE ATOMICITY, arch_console_write being a byte-at-a-time
# device loop under no lock, so a core printing a status line lands it INSIDE another's line
# character by character. The MARKER is a presence, so it is matched byte-exact first and a
# split is tolerated second, through require_on_wire, which reports the span it allowed.
#
# THE THREE READS THAT CARRY A VALUE STAY BYTE-EXACT, and that is not a gap: a bounded in-order
# match answers whether a literal reached the wire and never which value it carried, so one
# foreign digit inside a record would satisfy the named word with a longer address. All three
# tell a split apart from an absence instead, because the two send a reader to different places.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_kernel_half.sh <kernelhalf.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

need_qemu_machine
run_image "$elf"

# An absence, so weak by nature on a wire with more than one writer. Corroborated: a read that
# quietly returned leaves no marker, no ADDR at the word the image named and no fault status,
# and all three are asserted positively below.
if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi

addr="$(printf '%s\n' "$OUT" | sed -n 's/.*\[kernelhalf\] reading 0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ -z "$addr" ]; then
    printf '%s\n' "$OUT"
    if [ "$(wire_cores)" -gt 1 ] && wire_has "[kernelhalf] reading 0x"; then
        fail "the image's reading line reached the wire across $WIRE_SPAN bytes with a peer's
  line broken into it, so the word it names is UNREADABLE rather than unnamed. The address a
  shuffled line was going to name cannot be recovered from it"
    fi
    fail "the image named no address to read"
fi

require_single_marker "$marker" "reading the kernel's half did not fault"

field_matcher_control
require_field_on_wire ADDR "0x${addr}" 'ADDR=|ESR_EL1=' \
    "the record faults somewhere other than 0x${addr}, the word the image named"
require_field_on_wire ESR_EL1 0x9200000e 'ESR_EL1=' \
    "the syndrome is not a level-2 permission fault on a read from the lower level"

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${addr} in the kernel's half refused an unprivileged read (permission, level 2); exit $RC"
exit 0
