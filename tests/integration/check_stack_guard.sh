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
# counting. The syndrome must be a level-3 TRANSLATION fault on a WRITE taken from the lower
# exception level, which is what says the page carries no entry at all: a permission fault
# would mean the neighbour was mapped and merely read-only. And the image must print no ERROR
# line, so a write that quietly succeeded past the whole stack cannot pass as a fault.
#
# THE SYNDROME IS READ OFF THE THREAD-KILL RECORD AND NOT A PANIC DUMP. The walk is an EL0
# access, so armv8a now contains it: the same ESR and the same address arrive through
# kickos_fault_record as `ESR_EL1=` and `ADDR=` instead of the reporter's `ESR=` and `FAR=`
# (docs/design-m6-mmu.md F5, T8). The image dies of it and the system does not.
#
# ESR 0x92000047 is EC 0x24 (data abort, lower exception level), IL 1 (a 32-bit
# instruction), WnR set (a write) and DFSC 0b000111 (translation fault, level 3)
# (DDI 0487 M.b, ESR_EL1). Spelled out here rather than derived, so this gate asserts the
# encoding instead of restating whatever the source happens to produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_stack_guard.sh <stackguard.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

need_qemu_machine
run_image "$elf"

if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi

probes="$(printf '%s\n' "$OUT" | sed -n 's/.*\[stackguard\] touching 0x\([0-9a-f]*\).*/\1/p')"
count="$(printf '%s\n' "$probes" | grep -c '[0-9a-f]')"
if [ "$count" -lt 2 ]; then
    printf '%s\n' "$OUT"
    fail "only $count page(s) probed: the first probe faulted, so no page of the stack was ever written"
fi
last="$(printf '%s\n' "$probes" | tail -n 1)"

banners="$(printf '%s\n' "$OUT" | grep -c "$marker")"
if [ "$banners" -eq 0 ]; then
    fail "fault-dump marker '$marker' missing: walking below the stack did not fault"
fi
if [ "$banners" -ne 1 ]; then
    fail "fault-dump marker '$marker' appeared $banners times"
fi

if ! has "ADDR=0x${last}"; then
    printf '%s\n' "$OUT" | grep -E 'ADDR=|ESR_EL1='
    fail "the record faults somewhere other than 0x${last}, the page the image last named"
fi

if ! has "ESR_EL1=0x92000047"; then
    printf '%s\n' "$OUT" | grep -E 'ESR_EL1='
    fail "the syndrome is not a level-3 translation fault on a write from the lower level"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: $count page(s) of the stack written, then 0x${last} faulted (translation, level 3, write); exit $RC"
exit 0
