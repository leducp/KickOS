#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The stack-guard gate, RV64's own: run the `stackguard` image and assert that an unprivileged
# thread walking DOWN off the bottom of its own stack wrote several pages and then faulted, on
# the page it last named, with the cause a STORE from an unprivileged level gives.
#
# WHY THIS IS NOT THE AArch64 GATE WITH A DIFFERENT CONSTANT. That one asserts a syndrome
# carrying a translation-fault LEVEL and a write bit. On RISC-V there is no level field and no
# separate write bit: the CAUSE is the write bit, 15 being a store page fault where 13 is a
# load, and nothing beside it names a level or tells a missing leaf from a refusing one
# (RISC-V Privileged ISA, Supervisor Cause Register). What survives the port is the shape that
# matters here anyway: several pages written, then one that is not, at the address announced.
#
# The count floor is the load-bearing half and it is unchanged. A run whose FIRST probe faulted
# would satisfy every other assertion below while proving the opposite of the arm: that the
# thread never reached its own stack at all.
#
# scause 15 is a store page fault. Spelled out here rather than derived.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_stack_guard_rv64.sh <stackguard.elf> <dump-marker> <expect-status>"
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
    printf '%s\n' "$OUT" | grep -E 'ADDR=|scause='
    fail "the record faults somewhere other than 0x${last}, the page the image last named"
fi

if ! has "scause=0xf"; then
    printf '%s\n' "$OUT" | grep -E 'scause='
    fail "the cause is not a store page fault"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: $count page(s) of the stack written, then 0x${last} faulted (store page fault); exit $RC"
exit 0
