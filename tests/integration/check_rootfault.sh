#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ROOT-confinement gate: boot the `rootfault` image and assert that ROOT's write into a
# child's granted region TRAPS and is credited to root. The claim is about the thread that
# ran the ctors and the board bring-up, where check_mpu_fault.sh claims it for a spawned
# child. Native for the sim, QEMU when QEMU_MACHINE is set.
#
# Registered only on an enforcing build, so the app's own "NOT confined" line is a
# failure marker here.
#
# What a detected violation DOES is a property of the backend, so <outcome> is passed in.
# On the `thread-kill' arm root itself is what dies: kmain
# spawns root unprivileged, so the rule reaches it like any other thread. The child is
# still parked on its semaphore afterwards, which is what keeps the image alive and why
# that arm polls rather than waiting for an exit; the child outliving root IS the
# system-continues half of the claim.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_rootfault.sh <rootfault.elf> <outcome: panic|thread-kill>"
elf="${1:?$_usage}"
outcome="${2:?$_usage}"

case "$outcome" in
    panic)
        run_image "$elf"
        ;;
    thread-kill)
        poll_image "$elf" "child: wrote my own granted region" \
                          "$(thread_fault_re root)" "ADDR=0x"
        ;;
    *)
        fail "$_usage"
        ;;
esac

if has "ERROR"; then
    fail "rootfault reported a failed setup or control arm"
fi
if has "NOT confined"; then
    fail "root's cross-domain write was NOT trapped (enforcement off?)"
fi
# The CONTROL half must have run: the child writing its own granted region AND reading the
# value back proves the grant machinery worked, so the fault below is about root's
# confinement and not about a region that was never mapped.
if ! has "child: wrote my own granted region"; then
    fail "the child's write never took effect (setup failed before the test)"
fi

# Root must have reached the poke, so a fault cannot be credited to an earlier unrelated
# trap during ctors or bring-up; and the trap must be AT the region, not merely somewhere.
want="$(printf '%s\n' "$OUT" \
    | sed -n "s/.*root: writing the child's granted region at 0x\([0-9a-fA-F]*\).*/\1/p" \
    | head -n1)"
if [ -z "$want" ]; then
    fail "root never reached the deliberate write (faulted earlier?)"
fi
if [ "$outcome" = "thread-kill" ]; then
    if ! has_e "$(thread_fault_re root)"; then
        fail "no thread-kill for 'root' (crash / hang / truncated run?)"
    fi
    # The kill must be the WHOLE outcome: a redirect that fired and then escalated
    # anyway still prints the banner above.
    assert_no_panic "root's violation killed the thread AND panicked the system"
    # The app's "ERROR: child unparked" line, which its wait returning would print, is
    # covered by the ERROR check above.
elif ! has_e "MPU FAULT: thread 'root'|=== MPU FAULT ==="; then
    fail "MPU FAULT marker missing (crash / hang / truncated run?)"
fi
got="$(reported_fault_addr)"
if [ -z "$got" ]; then
    fail "the fault report carries no address (KICKOS_PANIC_DUMP off?)"
fi
if [ "$((0x$got))" -ne "$((0x$want))" ]; then
    fail "root trapped at 0x$got, not at the child's region 0x$want"
fi

echo "PASS: root took a memory trap on the cross-domain write at 0x$got ($outcome)"
exit 0
