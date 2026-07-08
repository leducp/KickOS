#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU TAP gate: boot the `selftest` image on QEMU (semihosting console), let the
# TAP suite run to completion, and assert a clean run -- "# all tests passed" with
# no "not ok". This is the SAME binary/suite that runs on the sim, now exercised on
# real ISA mechanism (armv7m/armv6m PendSV, rv32imac msip). selftest self-terminates
# (arch_shutdown when done), so QEMU_TIMEOUT is only a hang backstop. Tests that
# cannot run on a target self-skip via TAP `# SKIP` (still "ok").

set -u
elf="${1:?usage: check_qemu_selftest.sh <selftest.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

out="$(timeout "${QEMU_TIMEOUT:-30}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"

if echo "$out" | grep -q "not ok"; then
    echo "FAIL: a TAP test reported not ok"
    exit 1
fi
if echo "$out" | grep -q "# all tests passed"; then
    echo "PASS: selftest TAP suite clean"
    exit 0
fi
echo "FAIL: TAP completion marker missing (crash / hang / truncated run?)"
exit 1
