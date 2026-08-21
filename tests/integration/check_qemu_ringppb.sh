#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU PRIVILEGE-RING gate, fault arm: boot the `ringppb` image and assert that an
# unprivileged read of the privileged-only PPB (SCB->CPUID) TRAPS.
#
# Registered in BOTH postures, because the refusal is not the MPU's: ValidateAddress() takes
# the default system address map for any PPB access before consulting MPU_CTRL.ENABLE (ARM
# DDI 0403E.e B3.5.1/B3.5.3). That makes this the one confinement trap a no-MPU board can
# witness, so the app's own "NOT confined" line is a failure marker in every posture.
#
# What a detected violation DOES is a property of the backend, so <outcome> is passed in.
# The read happens in ROOT, which kmain spawns unprivileged in every posture, so on an
# isolating backend the BusFault kills root instead of panicking. Root is the only thread
# this image ever has, so exit_current ends the process either way and both arms can wait
# for an exit.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_qemu_ringppb.sh <ringppb.elf> <outcome: panic|thread-kill>"
elf="${1:?$_usage}"
outcome="${2:?$_usage}"

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
case "$outcome" in
    panic)
        if ! has_e "=== (HARD|MPU) FAULT ==="; then
            fail "no fault dump (the read completed silently, or the image hung)"
        fi
        ;;
    thread-kill)
        if ! has_e "$(thread_fault_re root)"; then
            fail "no thread-kill for 'root' (the read completed silently, or it hung?)"
        fi
        # The kill must be the WHOLE outcome: a redirect that fired and then escalated
        # anyway still prints the banner above.
        assert_no_panic "the PPB refusal killed root AND panicked the system"
        ;;
    *)
        fail "$_usage"
        ;;
esac

# A PPB permission failure sets a BFSR bit, and BFSR is CFSR[15:8]. Without this a stray
# UsageFault or a stacking fault elsewhere satisfies the banner grep above. Both dumps print
# CFSR under that name, the thread-kill one because arch_fault_redirect_to_exit captures it
# before clearing the sticky bits.
cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$cfsr" ]; then
    fail "fault dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
fi
if [ $(( (0x$cfsr >> 8) & 0xFF )) -eq 0 ]; then
    fail "CFSR=0x$cfsr has an empty BFSR byte: the trap was not a BusFault"
fi

# Pin the trap to the address the app probed. The address is printed only when the CFSR
# VALID bit is set, so its presence means it is live; requiring it to equal SCB->CPUID is
# what separates "the PPB refused THIS read" from any other precise BusFault on the way here.
#
# The claim is that a BUS fault recorded it, so the panic dump's address is read as BFAR by
# name rather than through reported_fault_addr(), which also accepts an MMFAR. The
# thread-kill dump prints one address under the neutral name ADDR, and
# arch_fault_redirect_to_exit takes MMFAR whenever MMARVALID is set and BFAR otherwise, so
# MMARVALID clear plus BFARVALID set is the same claim spelled in the CFSR.
if [ "$outcome" = "thread-kill" ]; then
    if [ $(( 0x$cfsr & 0x80 )) -ne 0 ]; then
        fail "CFSR=0x$cfsr has MMARVALID set: ADDR below is MMFAR, not the BusFault's"
    fi
    if [ $(( 0x$cfsr & 0x8000 )) -eq 0 ]; then
        fail "CFSR=0x$cfsr has BFARVALID clear: the BusFault recorded no address"
    fi
    bfar="$(printf '%s\n' "$OUT" | sed -n 's/.*ADDR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
else
    bfar="$(printf '%s\n' "$OUT" | sed -n 's/.*BFAR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
fi
if [ -z "$bfar" ]; then
    fail "no faulting address in the dump: the BusFault recorded none"
fi
if [ "$(( 0x$bfar ))" -ne "$(( 0xE000ED00 ))" ]; then
    fail "BusFault at 0x$bfar, not at SCB->CPUID 0xe000ed00"
fi

echo "PASS: the unprivileged PPB read took a precise BusFault at 0x$bfar (CFSR=0x$cfsr, $outcome)"
exit 0
