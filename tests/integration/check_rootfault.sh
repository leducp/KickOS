#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Task-confinement gate on the default system: boot the `rootfault` image and assert that
# MAIN's write into the region of a task it created TRAPS and is credited to main. The claim is
# about the default composition's task, where check_mpu_fault.sh claims it for a spawned child.
# Native for the sim, QEMU when QEMU_MACHINE is set.
#
# Registered only on an enforcing build, so the app's own "NOT confined" line is a
# failure marker here.
#
# What a detected violation DOES is a property of the backend, so <outcome> is passed in.
# On the `thread-kill' arm main is what dies, which ends its task, and the task the default
# composition `ends` on ending ends the system through the init: the status is KOS_EXIT_FAULT,
# never kfault_terminate's.

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
        run_image "$elf"
        ;;
    *)
        fail "$_usage"
        ;;
esac

if has "ERROR"; then
    fail "rootfault reported a failed setup or control arm"
fi
if has "NOT confined"; then
    fail "main's cross-domain write was NOT trapped (enforcement off?)"
fi
# The CONTROL half must have run: the child writing its own granted region AND reading the
# value back proves the grant machinery worked, so the fault below is about main's
# confinement and not about a region that was never mapped.
if ! has "child: wrote my own granted region"; then
    fail "the child's write never took effect (setup failed before the test)"
fi

# Main must have reached the poke, so a fault cannot be credited to an earlier unrelated
# trap during ctors or the init's walk; and the trap must be AT the region, not merely
# somewhere.
want="$(printf '%s\n' "$OUT" \
    | sed -n "s/.*main: writing the child's granted region at 0x\([0-9a-fA-F]*\).*/\1/p" \
    | head -n1)"
if [ -z "$want" ]; then
    fail "main never reached the deliberate write (faulted earlier?)"
fi
if [ "$outcome" = "thread-kill" ]; then
    if ! has_e "$(thread_fault_re main)"; then
        fail "no thread-kill for 'main' (crash / hang / truncated run?)"
    fi
    # The kill must be the WHOLE outcome: a redirect that fired and then escalated
    # anyway still prints the banner above.
    assert_no_panic "main's violation killed the thread AND panicked the system"
    # The app's "ERROR: child unparked" line, which its wait returning would print, is
    # covered by the ERROR check above.
    if [ "$RC" -eq 124 ]; then
        fail "the system never ended: the init did not act on main's fault"
    fi
    # KOS_EXIT_FAULT, restated rather than computed from the header the runtime reports.
    # A host SIGSEGV of the sim also exits 139; the kill record above tells the two apart.
    if [ "$RC" -ne 139 ]; then
        fail "the system ended with status $RC, not KOS_EXIT_FAULT (139)"
    fi
elif ! has_e "MPU FAULT: thread 'main'|=== MPU FAULT ==="; then
    fail "MPU FAULT marker missing (crash / hang / truncated run?)"
fi
got="$(reported_fault_addr)"
if [ -z "$got" ]; then
    fail "the fault report carries no address (KICKOS_PANIC_DUMP off?)"
fi
if [ "$((0x$got))" -ne "$((0x$want))" ]; then
    fail "main trapped at 0x$got, not at the child's region 0x$want"
fi

echo "PASS: main took a memory trap on the cross-domain write at 0x$got ($outcome)"
exit 0
