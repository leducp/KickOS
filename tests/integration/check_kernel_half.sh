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
# passing on a silent skip. The dump's fault address must equal that announcement, so a
# fault raised somewhere else cannot stand in. The syndrome must be a PERMISSION fault and
# not a translation fault: the kernel's half is mapped, at the same address, for the
# privileged side of the same core, so an image reporting a translation fault has lost the
# kernel's own window rather than revoked EL0's. And no ERROR line may appear, so a read
# that quietly returned a value cannot pass as a refusal.
#
# ESR 0x9200000e is EC 0x24 (data abort, lower exception level), IL 1 (a 32-bit
# instruction), WnR clear (a read) and DFSC 0b001110 (permission fault, level 2), which is
# the level of the 2 MiB block descriptor carrying the permission (DDI 0487 M.b, ESR_EL1).
# Spelled out here rather than derived, so this gate asserts the encoding instead of
# restating whatever the source happens to produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_kernel_half.sh <kernelhalf.elf> <dump-marker> <expect-status>"
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

if ! has "FAR=0x${addr}"; then
    printf '%s\n' "$OUT" | grep -E 'FAR=|ESR='
    fail "the dump faults somewhere other than 0x${addr}, the word the image named"
fi

if ! has "ESR=0x9200000e"; then
    printf '%s\n' "$OUT" | grep -E 'ESR='
    fail "the syndrome is not a level-2 permission fault on a read from the lower level"
fi

if [ "$RC" -ne "$expect_status" ]; then
    fail "expected exit $expect_status, got $RC"
fi
echo "PASS: 0x${addr} in the kernel's half refused an unprivileged read (permission, level 2); exit $RC"
exit 0
