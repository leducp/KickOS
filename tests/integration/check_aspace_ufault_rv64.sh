#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The translation-fault gate as the UNPRIVILEGED level sees it, RV64's own: run the
# `aspaceufault` image and assert that the process read a page the kernel had mapped into its
# space, that the same read faulted once the kernel unmapped it, and that it faulted at the
# page the kernel announced.
#
# The read must be the process's. This port never sets sstatus.SUM, so a supervisor load of a
# page carrying the unprivileged bit faults whether the leaf stands or not, with `scause` 13
# and the same `stval` either way (RISC-V Privileged ISA, Supervisor Cause Register): a
# kernel-side touch would satisfy every assertion below against a backend whose unmap did
# nothing. At the unprivileged level the read RETURNS while the leaf stands.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_ufault_rv64.sh <aspaceufault.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

run_faulting_image "$elf"

# The control must have run: a fault below is also what an address that was never mapped gives.
answers="$(printf '%s\n' "$OUT" | grep -c '\[aspaceufault\] the mapping answers')"
if [ "$answers" -ne 1 ]; then
    printf '%s\n' "$OUT"
    fail "the process never read the page while it was mapped ($answers control markers)"
fi

announce="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p')"
if [ -z "$announce" ]; then
    fail "the kernel never announced the page it unmapped, so no address comparison is possible"
fi

require_single_marker "$marker" "the unmapped page did not fault"
require_rv64_fault_at "$announce" "the page the kernel unmapped" \
    0xd "a load page fault" "$expect_status"
echo "PASS: 0x${announce} answered while mapped, then faulted (load page fault); exit $RC"
exit 0
