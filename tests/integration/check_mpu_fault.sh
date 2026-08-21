#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Memory-domain enforcement gate: boot the `mpu_fault` image and assert the deliberate
# CROSS-DOMAIN write traps and is attributed to the thread that made it. An unprivileged
# domain-A thread writes its own granted region (must succeed), then writes domain B's
# region (must fault). Native for the sim, QEMU when QEMU_MACHINE is set.
#
# Registered only on an enforcing build, where the cross-domain write is the one that traps.
#
# The banner alone is not the claim. Without the control marker AND the address pin
# below, a total grant failure (region A never granted at all) still passes: the fault
# then happens on the thread's OWN write, at a different address, and every marker the
# gate greps still appears.
#
# What a detected violation DOES is a property of the backend, so <outcome> is passed in.
# `panic' ends the system through kickos_isr_fault; `thread-kill' kills the worker alone, and
# root then parks forever on a semaphore nobody can post, so that arm polls and stops QEMU
# instead of waiting for an exit. The claim is the same either way: detected, and credited
# to 'domainA'.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_mpu_fault.sh <mpu_fault.elf> <outcome: panic|thread-kill>"
elf="${1:?$_usage}"
outcome="${2:?$_usage}"

case "$outcome" in
    panic)
        run_image "$elf"
        ;;
    thread-kill)
        poll_image "$elf" "\[domain\] A: my region ok" "$(thread_fault_re domainA)" "ADDR=0x"
        ;;
    *)
        fail "$_usage"
        ;;
esac

if has "ERROR"; then
    fail "mpu_fault reported a failed setup or control arm"
fi
if has_e "did not fault|cross-domain write completed"; then
    fail "the cross-domain write was NOT trapped (enforcement inactive?)"
fi
# The CONTROL half must have run: the thread writing its OWN granted region and reading the
# value back separates "domain B is refused" from "region A was never granted", which faults
# earlier and prints the same banner.
if ! has "\[domain\] A: my region ok"; then
    fail "the control write never took effect (region A not granted?)"
fi

# Pin the trap to the address the app announced, which is what the banner cannot say. The
# reporters differ in wording, RISC-V and the sim naming the thread from the kernel-reported
# path while ARM's panic dump names none and labels itself "MPU FAULT" only when the CFSR
# MMFSR byte is set, and every one of them records the faulting address.
want="$(printf '%s\n' "$OUT" \
    | sed -n 's/.*\[domain\] expect fault at 0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
if [ -z "$want" ]; then
    fail "the app never announced its expected fault address (faulted during setup?)"
fi
if [ "$outcome" = "thread-kill" ]; then
    if ! has_e "$(thread_fault_re domainA)"; then
        fail "no thread-kill for 'domainA' (crash / hang / truncated run?)"
    fi
    # The kill must be the WHOLE outcome: a redirect that fired and then escalated anyway
    # still shows the banner above, with the system it was meant to keep running dead.
    assert_no_panic "the domain violation killed the thread AND panicked the system"
elif ! has_e "MPU FAULT: thread 'domainA'|=== MPU FAULT ==="; then
    fail "MPU FAULT marker missing (crash / hang / truncated run?)"
fi
got="$(reported_fault_addr)"
if [ -z "$got" ]; then
    fail "the fault report carries no address (KICKOS_PANIC_DUMP off?)"
fi
if [ "$((0x$got))" -ne "$((0x$want))" ]; then
    fail "trap at 0x$got, not at domain B's 0x$want (the wrong write faulted)"
fi

echo "PASS: the unprivileged cross-domain write trapped at 0x$got ($outcome)"
exit 0
