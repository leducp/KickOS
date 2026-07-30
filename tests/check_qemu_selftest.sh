#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU TAP gate: boot the `selftest` image (semihosting console) and hand the stream to
# tests/check_tap_stream.sh, which owns the verdict. selftest self-terminates, so
# QEMU_TIMEOUT is only a hang backstop.

set -u
elf="${1:?usage: check_qemu_selftest.sh <selftest.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)
here="$(cd "$(dirname "$0")" && pwd)"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

out="$(timeout "${QEMU_TIMEOUT:-30}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1)"
echo "$out"
printf '%s\n' "$out" | "$here/check_tap_stream.sh" "qemu/$machine"
