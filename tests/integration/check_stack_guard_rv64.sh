#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The stack-guard gate, RV64's own: run the `stackguard` image and assert that an unprivileged
# thread walking DOWN off the bottom of its own stack wrote several pages and then faulted, on
# the page it last named, with the cause a STORE from an unprivileged level gives.
#
# The cause carries the write bit and nothing else: 15 is a store page fault where 13 is a
# load, and no field beside it names a level or tells a missing leaf from a refusing one
# (RISC-V Privileged ISA, Supervisor Cause Register).
#
# Without the count floor, a run whose first probe faulted passes every other assertion here.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_stack_guard_rv64.sh <stackguard.elf> <dump-marker> <expect-status>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
expect_status="${3:?$_usage}"

run_faulting_image "$elf"

probes="$(printf '%s\n' "$OUT" | sed -n 's/.*\[stackguard\] touching 0x\([0-9a-f]*\).*/\1/p')"
count="$(printf '%s\n' "$probes" | grep -c '[0-9a-f]')"
if [ "$count" -lt 2 ]; then
    printf '%s\n' "$OUT"
    fail "only $count page(s) probed: the first probe faulted, so no page of the stack was ever written"
fi
last="$(printf '%s\n' "$probes" | tail -n 1)"

require_single_marker "$marker" "walking below the stack did not fault"
require_rv64_fault_at "$last" "the page the image last named" \
    0xf "a store page fault" "$expect_status"
echo "PASS: $count page(s) of the stack written, then 0x${last} faulted (store page fault); exit $RC"
exit 0
