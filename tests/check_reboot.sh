#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Plumbing gate for kos_reboot: boot rebootdemo on a declining-fallback target (no QEMU
# machine models a bootrom download mode), so the whole path (root's kos_reboot,
# the AUTH_SYSTEM gate, the synchronous console flush, the arch_reboot fallback) must
# come back with -KOS_ENOSYS and the app must then shut down cleanly. arch_shutdown
# forwards the status over semihosting, so QEMU's exit code IS the clean-exit half.
# Native run for the sim; QEMU (semihosting) when QEMU_MACHINE is set.
#
# rc=-38 is -KOS_ENOSYS (system/include/kickos/sys/errno.h).

set -u
. "$(dirname "$0")/lib/gate.sh"

elf="${1:?usage: check_reboot.sh <rebootdemo.elf>}"

run_image "$elf"

assert_no_panic "panic/fault during rebootdemo"
if [ "$RC" -eq 124 ]; then
    fail "rebootdemo timed out (no exit status forwarded)"
fi
if [ "$RC" -ne 0 ]; then
    fail "rebootdemo exit status $RC (expected a clean shutdown)"
fi
if ! has "reboot declined: rc=-38"; then
    fail "the -KOS_ENOSYS refusal line is missing"
fi
echo "PASS: kos_reboot declined with -KOS_ENOSYS and the app shut down cleanly"
exit 0
