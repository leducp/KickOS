#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU PRIVILEGE-RING gate, fault arm: boot the `ringppb` image and assert that an
# unprivileged read of the privileged-only PPB (SCB->CPUID) TRAPS.
#
# Unlike check_qemu_rootfault.sh this is registered WITHOUT KICKOS_HAVE_MPU, because the
# refusal is not the MPU's: ValidateAddress() takes the default system address map for any
# PPB access before consulting MPU_CTRL.ENABLE (ARM DDI 0403E.e B3.5.1/B3.5.3). That is
# what makes this the one confinement trap a no-MPU board can witness, so the app's own
# "NOT confined" line is a failure marker in every posture.

set -u
elf="${1:?usage: check_qemu_ringppb.sh <ringppb.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    echo "SKIP: $qemu not found"
    exit 77
fi

# shellcheck disable=SC2086
out="$(timeout "${QEMU_TIMEOUT:-20}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1 | tr -d '\r')"
echo "$out"

if echo "$out" | grep -qE "\[ringppb\] ERROR|NOT confined"; then
    echo "FAIL: the unprivileged PPB read was NOT refused"
    exit 1
fi
# The control half must have run: an identical volatile load of held memory succeeding is
# what separates "the PPB is refused" from "loads are broken in this image".
if ! echo "$out" | grep -q "ok - control: a 32-bit volatile load"; then
    echo "FAIL: the control load never succeeded (setup failed before the test)"
    exit 1
fi
# ...and root must have reached the deliberate read, so the trap cannot be credited to an
# earlier unrelated fault during ctors or board bring-up.
if ! echo "$out" | grep -q "root: reading privileged-only SCB->CPUID"; then
    echo "FAIL: root never reached the deliberate PPB read (faulted earlier?)"
    exit 1
fi
if ! echo "$out" | grep -qE "=== (HARD|MPU) FAULT ==="; then
    echo "FAIL: no fault dump (the read completed silently, or the image hung)"
    exit 1
fi

# Discriminate a genuine BUS fault from any other trap that also prints this banner. A
# PPB permission failure sets a BFSR bit, and BFSR is CFSR[15:8]; without this check a
# stray UsageFault or a stacking fault elsewhere would satisfy the banner grep above.
cfsr="$(echo "$out" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$cfsr" ]; then
    echo "FAIL: fault dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
    exit 1
fi
if [ $(( (0x$cfsr >> 8) & 0xFF )) -eq 0 ]; then
    echo "FAIL: CFSR=0x$cfsr has an empty BFSR byte: the trap was not a BusFault"
    exit 1
fi

# Pin the trap to the address the app actually probed. The dump prints BFAR only when the
# CFSR BFARVALID bit is set, so its presence already means the address is live rather than
# stale; requiring it to equal SCB->CPUID is what separates "the PPB refused THIS read"
# from any other precise BusFault the image might have taken on the way here.
bfar="$(echo "$out" | sed -n 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$bfar" ]; then
    echo "FAIL: no BFAR in the dump: the BusFault recorded no faulting address"
    exit 1
fi
if [ "$(( 0x$bfar ))" -ne "$(( 0xE000ED00 ))" ]; then
    echo "FAIL: BusFault at 0x$bfar, not at SCB->CPUID 0xe000ed00"
    exit 1
fi

echo "PASS: the unprivileged PPB read took a precise BusFault at 0x$bfar (CFSR=0x$cfsr)"
exit 0
