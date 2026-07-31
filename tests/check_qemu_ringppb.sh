#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU PRIVILEGE-RING gate, fault arm: boot the `ringppb` image and assert that an
# unprivileged read of the privileged-only PPB (SCB->CPUID) TRAPS.
#
# Unlike check_rootfault.sh this is registered WITHOUT KICKOS_HAVE_MPU, because the
# refusal is not the MPU's: ValidateAddress() takes the default system address map for any
# PPB access before consulting MPU_CTRL.ENABLE (ARM DDI 0403E.e B3.5.1/B3.5.3). That is
# what makes this the one confinement trap a no-MPU board can witness, so the app's own
# "NOT confined" line is a failure marker in every posture.

set -u
. "$(dirname "$0")/lib/gate.sh"

elf="${1:?usage: check_qemu_ringppb.sh <ringppb.elf>}"

need_qemu_machine
run_image "$elf"

if has_e "\[ringppb\] ERROR|NOT confined"; then
    fail "the unprivileged PPB read was NOT refused"
fi
# The control half must have run: an identical volatile load of held memory succeeding is
# what separates "the PPB is refused" from "loads are broken in this image".
if ! has "ok - control: a 32-bit volatile load"; then
    fail "the control load never succeeded (setup failed before the test)"
fi
# ...and root must have reached the deliberate read, so the trap cannot be credited to an
# earlier unrelated fault during ctors or board bring-up.
if ! has "root: reading privileged-only SCB->CPUID"; then
    fail "root never reached the deliberate PPB read (faulted earlier?)"
fi
if ! has_e "=== (HARD|MPU) FAULT ==="; then
    fail "no fault dump (the read completed silently, or the image hung)"
fi

# Discriminate a genuine BUS fault from any other trap that also prints this banner. A
# PPB permission failure sets a BFSR bit, and BFSR is CFSR[15:8]; without this check a
# stray UsageFault or a stacking fault elsewhere would satisfy the banner grep above.
cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$cfsr" ]; then
    fail "fault dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
fi
if [ $(( (0x$cfsr >> 8) & 0xFF )) -eq 0 ]; then
    fail "CFSR=0x$cfsr has an empty BFSR byte: the trap was not a BusFault"
fi

# Pin the trap to the address the app actually probed. The dump prints BFAR only when the
# CFSR BFARVALID bit is set, so its presence already means the address is live rather than
# stale; requiring it to equal SCB->CPUID is what separates "the PPB refused THIS read"
# from any other precise BusFault the image might have taken on the way here.
# Read BFAR by name rather than through reported_fault_addr(), which would also accept
# an MMFAR: this gate's claim is specifically that a BUS fault recorded the address.
bfar="$(printf '%s\n' "$OUT" | sed -n 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$bfar" ]; then
    fail "no BFAR in the dump: the BusFault recorded no faulting address"
fi
if [ "$(( 0x$bfar ))" -ne "$(( 0xE000ED00 ))" ]; then
    fail "BusFault at 0x$bfar, not at SCB->CPUID 0xe000ed00"
fi

echo "PASS: the unprivileged PPB read took a precise BusFault at 0x$bfar (CFSR=0x$cfsr)"
exit 0
