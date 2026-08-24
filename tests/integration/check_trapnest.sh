#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NESTED-TRAP witness (rv32imac). The claim: an interrupt taken while the kernel is already
# running a thread's syscall dispatch must NOT put its frame on that thread's stack.
#
# THAT IS A PRIVILEGE CLAIM. rv32imac runs syscall_dispatch privileged, in thread mode, on the
# caller's own continuation, and such an interrupt arrives with mstatus.MPP=M, so no U-mode sp
# check applies to it: a frame built on the sp it finds lands at whatever depth the dispatch
# had reached, and if that sp were the calling THREAD'S the kernel would be writing below its
# own stack_lo, privileged, where a neighbour's granted region is. Two things hold that off,
# and this arm covers both: the entry transfers a U-mode ecall to the thread's own KERNEL
# stack, so the dispatch is not on the thread's stack to begin with, and .Ltrap_from_m keeps
# only msip and ecall-from-M on the interrupted sp.
#
# THE WORKER PARKS ITS SP LOW ON PURPOSE, at the room a regressed entry would need for the
# ecall frame, the dispatch and the msip frame under it. Deeper than that, a regression's
# frames land BELOW stack_lo, where the in-bounds test below cannot see them and the arm would
# report a false pass; the low edge itself is witnessed by the faultsurvive lowedge arm.
#
# WHAT MAKES IT DETERMINISTIC. The app does not wait for a tick: kos_irq_inject raises its
# line from inside the dispatch with interrupts enabled, so the trap fires at that exact
# instruction on every call: one nested M-mode trap per call, no window to hit. The kernel
# tallies them (kickos_nestwitness_note) and ROOT reads the tally back through
# KOS_SYS_NEST_WITNESS and prints it, so this gate reads an OBSERVATION rather than
# inferring one from corrupted memory. The print has to be the app's: a kprintf on
# kickos_terminate puts the console's varargs route inside the syscall descent and deepens
# what that class measures.
#
# THE TWO CLAUSES, and neither works without the other:
#   traps > 0    the positive control. onstack == 0 with nothing provoked is what a deleted
#                inject, a masked line or a dead app all look like, and it would pass forever.
#   onstack == 0 the claim itself.
#
# THE THIRD LINE IS REPORTED, NOT ASSERTED. When a frame did land on a thread stack the
# kernel also prints how much room was left below it. The figure to compare that against is
# the interrupt KERNEL DESCENT alone: the frame itself is already spent, so what has to fit
# under it is the ISR. It cannot be a clause: a fixed system prints no such line, so requiring
# it would make the arm unfalsifiable in the direction that matters.
#
# QEMU, machine from kickos_add_qemu_test.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_trapnest.sh <elf> <interrupt-kernel-descent-bytes>"
elf="${1:?$_usage}"
descent="${2:?$_usage}"

case "$descent" in
    ''|*[!0-9]*) fail "the descent argument '$descent' is not a number" ;;
    *) ;;
esac

run_image "$elf"

if has "\[trapnest\] ERROR"; then
    fail "the app reported its own failure"
fi
if ! has "\[trapnest\] worker done"; then
    fail "the worker never finished its injects"
fi
if ! has "\[trapnest\] root ran after the worker"; then
    fail "root never ran again, so the join never returned"
fi
assert_no_panic "the arm ended in a panic"

line="$(printf '%s\n' "$OUT" | grep -E '^\[nestwitness\] traps=' | head -n1)"
[ -n "$line" ] || fail "no [nestwitness] tally in the output: KICKOS_ENABLE_SELFTEST off, so
    KOS_SYS_NEST_WITNESS is not in the dispatch and the counters are not compiled"

traps="$(printf '%s\n' "$line" | sed -n 's/.*traps=\([0-9]\{1,\}\).*/\1/p')"
onstack="$(printf '%s\n' "$line" | sed -n 's/.*onstack=\([0-9]\{1,\}\).*/\1/p')"
[ -n "$traps" ] && [ -n "$onstack" ] || fail "cannot read both counters out of: $line"

if [ "$traps" -eq 0 ]; then
    fail "traps=0: no interrupt was taken while the kernel was running, so this arm
    observed nothing. The inject line is masked (irq_attach refused?), the inject syscall
    is compiled out, or the witness is no longer called."
fi

room="$(printf '%s\n' "$OUT" \
        | sed -n 's/^\[nestwitness\] closest frame sat \([0-9]\{1,\}\) bytes above stack_lo$/\1/p' \
        | head -n1)"

if [ "$onstack" -ne 0 ]; then
    why="of $traps nested traps, $onstack built their frame on the interrupted thread's stack"
    if [ -n "$room" ]; then
        why="$why; the closest sat $room bytes above stack_lo, against a worst-case interrupt descent of $descent below it"
        if [ "$room" -lt "$descent" ]; then
            why="$why, so the ISR under that frame ran off the bottom of the stack"
        fi
    fi
    fail "NESTED FRAME ON A THREAD STACK: $why"
fi

if [ -n "$room" ]; then
    fail "onstack=0 yet the kernel reported a room figure, which it only records for a frame
    it saw on a thread stack: the two counters disagree and one of them is not being written"
fi

echo "PASS: $traps nested M-mode traps, none of them on the interrupted thread's stack"
exit 0
