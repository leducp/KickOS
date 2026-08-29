#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel-half revocation gate, RV64's own: run the `kernelhalf` image and assert that an
# unprivileged thread reading a word of kernel writable state faulted, at the address the image
# named, with the cause a LOAD from an unprivileged level gives.
#
# WHY THIS IS NOT THE AArch64 GATE WITH A DIFFERENT CONSTANT, which is the honest reason to
# write a second script instead of parameterising the first. That gate's decisive assertion is
# that the syndrome names a PERMISSION fault and not a translation fault: the kernel's half is
# mapped at that same address for the privileged side of the same core, so a translation fault
# there would mean the kernel's own window had been lost rather than the unprivileged level's
# revoked. RISC-V CANNOT MAKE THAT DISTINCTION AT ALL. `scause` carries 13 for every load page
# fault, whether the leaf is absent, whether it is present without R, or whether it is present
# with U clear and the access came from U-mode; there is no fault-status field and no level
# field beside it (RISC-V Privileged ISA, Supervisor Cause Register). Substituting a constant
# would have looked like a port and would have quietly dropped the one thing the arm is for.
#
# WHAT REPLACES IT, and it is weaker on purpose rather than by omission. Two facts stand in for
# the permission/translation split, and the gate asserts both:
#   - the kernel's own window is demonstrably ALIVE at the moment of the fault, because the
#     record below is printed by kernel text reading kernel rodata and the system continues
#     afterwards. An image that had lost the kernel's window could not report at all; that is
#     R1.2's silence, and this gate's marker count is what separates the two.
#   - the fault is CONTAINED to the thread rather than fatal to the machine, which is the
#     thread-kill record and the exit status, and which only a live kernel half produces.
# What is NOT claimed is that the leaf exists and refuses; on this architecture no instrument
# on the machine can tell that from the leaf being absent.
#
# THE CAUSE IS READ OFF THE THREAD-KILL RECORD AND NOT A PANIC DUMP. The read is an unprivileged
# access, so fault isolation contains it: `scause` and `stval` arrive through
# kickos_fault_record as `scause=` and `ADDR=` (docs/design-m6-mmu.md F5, R1.5).
#
# scause 13 is a load page fault. Spelled out here rather than derived, so this gate asserts
# the encoding instead of restating whatever the source happens to produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_kernel_half_rv64.sh <kernelhalf.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

need_qemu_machine
run_image "$elf"

if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi

addr="$(printf '%s\n' "$OUT" | sed -n 's/.*\[kernelhalf\] reading 0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ -z "$addr" ]; then
    printf '%s\n' "$OUT"
    fail "the image named no address to read"
fi

banners="$(printf '%s\n' "$OUT" | grep -c "$marker")"
if [ "$banners" -eq 0 ]; then
    fail "fault-dump marker '$marker' missing: reading the kernel's half did not fault"
fi
if [ "$banners" -ne 1 ]; then
    fail "fault-dump marker '$marker' appeared $banners times"
fi

if ! has "ADDR=0x${addr}"; then
    printf '%s\n' "$OUT" | grep -E 'ADDR=|scause='
    fail "the record faults somewhere other than 0x${addr}, the word the image named"
fi

if ! has "scause=0xd"; then
    printf '%s\n' "$OUT" | grep -E 'scause='
    fail "the cause is not a load page fault"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${addr} in the kernel's half refused an unprivileged read (load page fault); exit $RC"
exit 0
