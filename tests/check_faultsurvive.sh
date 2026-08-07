#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-isolation witness. Two arms, two images, two opposite claims.
#
# `survive' (faultsurvive, KICKOS_FS_MODE 0): an unprivileged worker executes an
# undefined instruction and only that thread dies. The claim is ORDERING, not presence:
# both lines appearing proves nothing, since a system that panicked after printing them
# would look the same. Root is parked in join() until the worker is gone, so the
# survivor's line coming after the kill banner is causal, not a race that happened to
# land the right way round.
#
# `overflow' (faultsurvive_ovf, KICKOS_FS_MODE 1): the worker recurses off its stack, and
# the fault must reach the PANIC dump (design 4.2) rather than the thread kill. How the
# backend knows differs, so the corroborating evidence differs with it:
#   armv7m   hardware stacking aborts before the handler runs, so no frame was written
#            at all; CFSR MSTKERR is the tell.
#   rv32imac the trap prologue is software and runs M-mode, which bypasses the unlocked
#            PMP entries, so the frame IS written - below the thread's stack. Nothing
#            latches; the store access fault on the recursion's own push is the tell, and
#            the stack-bounds test is the only thing that refuses the frame.
#
# `offstack' (faultsurvive_off, KICKOS_FS_MODE 2): the worker points SP at a buffer
# outside its stack and faults there. The frame is complete and the thread really is
# unprivileged in thread mode, so every register-derived clause says yes and NO status
# bit is set on any backend: the stack-bounds test alone can refuse it. Delete that test
# and this arm reports a thread kill instead of a panic, which is what makes it the
# witness that the backend calls kickos_fault_frame_trusted at all.
#
# Both refusal arms share the negative claim: no kill banner, a panic dump, exit 132.
#
# QEMU, machine from kickos_add_qemu_test; the survive arm also runs natively on the sim.

set -u
. "$(dirname "$0")/lib/gate.sh"

_usage="usage: check_faultsurvive.sh <elf> <arm: survive|overflow|offstack> [arch]"
elf="${1:?$_usage}"
arm="${2:?$_usage}"
# Passed, not sniffed out of the dump: which fact corroborates a refusal is a property of
# the backend, and a gate that read it back from the output it is judging would accept
# whichever dump it got.
arch="${3:-armv7m}"

run_image "$elf"

if has "\[fs\] ERROR"; then
    fail "the app reported its own failure"
fi
if ! has "\[fs\] worker about to fault"; then
    fail "the worker never reached its deliberate fault"
fi

# First matching line number, so the two arms can be ordered against each other.
line_of() { printf '%s\n' "$OUT" | grep -nE "$1" | head -n1 | cut -d: -f1; }

case "$arm" in
    survive)
        killed="$(line_of "$(thread_fault_re faulter)")"
        if [ -z "$killed" ]; then
            fail "no thread-kill for 'faulter' (the fault ended the system?)"
        fi
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            fail "root never ran again after the worker faulted"
        fi
        if [ "$survived" -le "$killed" ]; then
            fail "root's line is at $survived, not after the kill at $killed"
        fi
        assert_no_panic "the worker was killed AND the system panicked"
        if [ "$RC" -ne 0 ]; then
            fail "expected a clean exit 0 once root returned, got $RC"
        fi
        echo "PASS: 'faulter' died at line $killed and root ran at line $survived"
        ;;
    overflow | offstack)
        if has_e "$(thread_fault_re faulter)"; then
            fail "$arm: the fault was redirected to the exit stub instead of escalating"
        fi
        if ! has_e "$KOS_PANIC_RE"; then
            fail "$arm: the fault reached no dump (looped, or walked into a neighbour?)"
        fi
        # Corroboration, so the arm cannot pass on ANY panic that happens to occur.
        why=""
        case "$arch:$arm" in
            armv7m:overflow)
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || fail "the dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$cfsr & 0x10 )) -eq 0 ]; then
                    fail "CFSR=0x$cfsr has MSTKERR clear: this was not a stacking failure"
                fi
                why="CFSR=0x$cfsr, MSTKERR"
                ;;
            armv7m:offstack)
                # The whole point: the frame WAS written, so the bits the CFSR early-out
                # would have refused on are exactly the ones that must be clear. Without
                # this the arm could pass on a stacking abort and say nothing about the
                # bounds test.
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || fail "the dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$cfsr & 0x1818 )) -ne 0 ]; then
                    fail "CFSR=0x$cfsr carries a stacking-abort bit, so the CFSR early-out could have refused this frame and the arm proves nothing about the bounds test"
                fi
                why="CFSR=0x$cfsr, no stacking-abort bit"
                ;;
            rv32imac:overflow)
                # M-mode bypasses the unlocked PMP entries, so the trap prologue writes
                # the frame below the stack and nothing latches. What the recursion's OWN
                # push did is the evidence, and it is a U-mode store the PMP denied.
                if ! has "MPU FAULT: task 'faulter' attempted write"; then
                    fail "no denied write credited to 'faulter': the recursion never ran off its granted stack"
                fi
                why="PMP-denied write by 'faulter'"
                ;;
            rv32imac:offstack)
                has "RISC-V TRAP (illegal instruction)" \
                  || fail "the dump names a cause other than the deliberate illegal instruction"
                # MPP == 0 (mstatus bits 12:11) is the privilege clause reporting that it
                # said YES. With no status register to latch a bad frame, the stack-bounds
                # test is then the only thing left that can have refused this.
                mst="$(printf '%s\n' "$OUT" | sed -n 's/.*mstatus=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$mst" ] || fail "the dump carries no mstatus (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$mst & 0x1800 )) -ne 0 ]; then
                    fail "mstatus=0x$mst has MPP != U: the fault was not taken in user mode, so the privilege clause refused it and the arm proves nothing about the bounds test"
                fi
                why="mstatus=0x$mst, MPP=U"
                ;;
            *)
                fail "$arm: no corroborating evidence is defined for arch '$arch'"
                ;;
        esac
        # Which dead-end ran: the task-naming reporter ends the system with 0, the shared
        # kfault_terminate with 132. Both are the unchanged pre-isolation behaviour for
        # their backend, so the arm pins whichever one this fault reached.
        want=132
        if has "MPU FAULT: task"; then
            want=0
        fi
        if [ "$RC" -ne "$want" ]; then
            fail "expected exit $want from the escalation path, got $RC"
        fi
        echo "PASS: the $arm fault escalated to the panic dump ($why, exit $RC)"
        ;;
    *)
        fail "$_usage"
        ;;
esac
exit 0
