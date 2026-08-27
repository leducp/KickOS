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
# where the read returned, so a silent zero cannot pass as a fault. The dump's FAR must equal
# the address the kernel announced before it switched, so a fault somewhere else (a stray
# access on the way, or the wrong page unmapped) fails rather than counting. And the syndrome
# must be a TRANSLATION fault taken at the current exception level rather than a permission
# fault or an alignment fault, which would mean the entry was still there.
#
# ESR 0x96000007 is EC 0x25 (data abort, no change in exception level), IL 1 (a 32-bit
# instruction) and DFSC 0b000111 (translation fault, level 3), with WnR clear for a read
# (DDI 0487 M.b, ESR_EL1). Spelled out here rather than derived, so this gate asserts the
# encoding instead of restating whatever the source happens to produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_fault.sh <aspacefault.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

need_qemu_machine
run_image "$elf"

if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi

announce="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p')"
if [ -z "$announce" ]; then
    fail "the kernel never announced the page it unmapped, so no FAR comparison is possible"
fi

count="$(printf '%s\n' "$OUT" | grep -c "$marker")"
if [ "$count" -eq 0 ]; then
    fail "fault-dump marker '$marker' missing: the unmapped page did not fault"
fi
if [ "$count" -ne 1 ]; then
    fail "fault-dump marker '$marker' appeared $count times"
fi

if ! has "FAR=0x${announce}"; then
    printf '%s\n' "$OUT" | grep -E 'FAR=|ESR='
    fail "the dump faults somewhere other than 0x${announce}, the page the kernel unmapped"
fi

if ! has "ESR=0x96000007"; then
    printf '%s\n' "$OUT" | grep -E 'ESR='
    fail "the syndrome is not a level-3 translation fault on a read at the current level"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${announce} faulted (translation, level 3) and the system stopped with $RC"
exit 0
