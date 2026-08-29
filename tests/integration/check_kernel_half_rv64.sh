#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel-half revocation gate, RV64's own: run the `kernelhalf` image and assert that an
# unprivileged thread reading a word of kernel writable state faulted, at the address the image
# named, with the cause a LOAD from an unprivileged level gives.
#
# `scause` carries 13 for every load page fault, whether the leaf is absent, present without R,
# or present with U clear against a U-mode access, and no field beside it names a level or a
# fault status (RISC-V Privileged ISA, Supervisor Cause Register). There is no
# permission/translation split to assert here.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_kernel_half_rv64.sh <kernelhalf.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

run_faulting_image "$elf"

addr="$(printf '%s\n' "$OUT" | sed -n 's/.*\[kernelhalf\] reading 0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ -z "$addr" ]; then
    printf '%s\n' "$OUT"
    fail "the image named no address to read"
fi

require_single_marker "$marker" "reading the kernel's half did not fault"
require_rv64_fault_at "$addr" "the word the image named" \
    0xd "a load page fault" "$expect_status"
echo "PASS: 0x${addr} in the kernel's half refused an unprivileged read (load page fault); exit $RC"
exit 0
