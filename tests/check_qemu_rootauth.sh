#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU ROOT-narrow gate: boot the `rootauth` image and assert its verdict.
#
# A grep for the absence of ERROR cannot tell a truncated run from a clean one, so each
# arm must be seen. rootauth self-terminates, so QEMU_TIMEOUT is only a hang backstop.

set -u
elf="${1:?usage: check_qemu_rootauth.sh <rootauth.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-20}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1 | tr -d '\r')"
echo "$out"

if echo "$out" | grep -qE "\[rootauth\] (ERROR|FAIL)"; then
    echo "FAIL: rootauth reported a failed arm"
    exit 1
fi
if ! echo "$out" | grep -q "\[rootauth\] PASS"; then
    echo "FAIL: rootauth verdict line missing (crash / hang / truncated run?)"
    exit 1
fi
# The verdict prints before main returns, and the return calls kos_shutdown (gated on
# KOS_AUTH_SYSTEM), so losing only that bit prints PASS and then panics. Text from the
# kos_panic call in kernel/init/kmain.cc.
if echo "$out" | grep -q "root: shutdown refused"; then
    echo "FAIL: rootauth panicked after its verdict (KOS_AUTH_SYSTEM lost?)"
    exit 1
fi

# EXACTLY five arms, all unconditional (rootauth main.cc). The floor must equal that
# count: any slack lets an arm be deleted with every gate still green, and the arm most
# worth deleting is the post-narrow refusal, the only one separating a narrow that took
# effect from one that returned 0 and changed nothing. Raise this with the arm count.
arms="$(echo "$out" | grep -c "\[rootauth\] ok - ")"
if [ "$arms" -lt 5 ]; then
    echo "FAIL: only $arms rootauth arm(s) reported; the verdict is vacuous"
    exit 1
fi

echo "PASS: rootauth clean ($arms arms)"
exit 0
