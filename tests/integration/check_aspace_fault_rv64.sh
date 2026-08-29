#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The translation-fault gate, RV64's own: run the `aspacefault` image and assert that the
# KERNEL's own read of a page it had just unmapped faulted, at the page it announced, with the
# cause a load from a page with no translation gives.
#
# A DYING-IMAGE GATE, not a contained one. The access is the kernel's own inside
# KOS_ASPACE_OP_TOUCH_UNMAPPED, so fault isolation does not claim it and what reaches the
# console is the reporter's panic dump: the banner, then `scause=` and `stval=`, where the
# contained record would carry `scause=` and `ADDR=`.
#
# WHY THIS IS NOT THE AArch64 GATE WITH A DIFFERENT CONSTANT. That one asserts a syndrome whose
# fault-status field names a translation fault at level 3, which is what distinguishes "no leaf"
# from "a leaf that refuses". RISC-V publishes neither: `scause` is 13 for every load page
# fault at any level and for any reason, and `stval` carries the address alone (RISC-V
# Privileged ISA, Supervisor Cause Register). So the assertion here is the CAUSE plus the
# ADDRESS, and the level claim is dropped rather than faked.
#
# The address comparison is what keeps that honest: `stval` must equal the page the kernel said
# it had unmapped, so a fault raised anywhere else, by anything else, cannot stand in.
#
# scause 13 is a load page fault. Spelled out here rather than derived.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_fault_rv64.sh <aspacefault.elf> <dump-marker> <expect-status>"
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
    fail "the kernel never announced the page it unmapped, so no address comparison is possible"
fi

count="$(printf '%s\n' "$OUT" | grep -c "$marker")"
if [ "$count" -eq 0 ]; then
    fail "fault-dump marker '$marker' missing: the unmapped page did not fault"
fi
if [ "$count" -ne 1 ]; then
    fail "fault-dump marker '$marker' appeared $count times"
fi

if ! has "stval=0x${announce}"; then
    printf '%s\n' "$OUT" | grep -E 'stval=|scause='
    fail "the dump faults somewhere other than 0x${announce}, the page the kernel unmapped"
fi

if ! has "scause=0xd"; then
    printf '%s\n' "$OUT" | grep -E 'scause='
    fail "the cause is not a load page fault"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${announce} faulted (load page fault) and the system stopped with $RC"
exit 0
