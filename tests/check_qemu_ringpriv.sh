#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU PRIVILEGE-RING gate: boot the `ringpriv` image and assert its verdict.
#
# Registered on enforcing AND non-enforcing builds on purpose. The ring is the fabricated
# first frame's CONTROL.nPRIV, not an MPU feature, so every arm must hold identically at
# -DKICKOS_HAVE_MPU=0 and =1; a posture-dependent result here would itself be the bug.
#
# The arm floor comes from the caller (its CMakeLists knows the board's posture and hence
# how many arms are unconditional), because a grep for the absence of ERROR cannot tell a
# truncated run from a clean one. ringpriv self-terminates, so QEMU_TIMEOUT is only a hang
# backstop.

set -u
elf="${1:?usage: check_qemu_ringpriv.sh <ringpriv.elf> <expected-arms>}"
want_arms="${2:?usage: check_qemu_ringpriv.sh <ringpriv.elf> <expected-arms>}"
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

if echo "$out" | grep -qE "\[ringpriv\] (ERROR|FAIL)"; then
    echo "FAIL: ringpriv reported a failed arm"
    exit 1
fi
if ! echo "$out" | grep -q "\[ringpriv\] PASS"; then
    echo "FAIL: ringpriv verdict line missing (crash / hang / truncated run?)"
    exit 1
fi

# The floor must be MET EXACTLY, not merely reached. Too few means an arm was dropped or
# the run was cut short; too many means the posture changed under the gate (an armv6m
# no-ring board reporting the full armv7m set, say) without the expectation being updated.
arms="$(echo "$out" | grep -c "\[ringpriv\] ok - ")"
if [ "$arms" -ne "$want_arms" ]; then
    echo "FAIL: ringpriv reported $arms arm(s), expected exactly $want_arms"
    exit 1
fi

# The verdict's own arm count must agree with the lines on the wire. These are produced by
# different code paths (a counter vs one emit per arm), so a mismatch means output was lost
# between them and the tally above cannot be trusted.
if ! echo "$out" | grep -q "\[ringpriv\] PASS ($want_arms arms)"; then
    echo "FAIL: ringpriv verdict does not claim $want_arms arms (output lost?)"
    exit 1
fi

echo "PASS: ringpriv clean ($arms arms)"
exit 0
