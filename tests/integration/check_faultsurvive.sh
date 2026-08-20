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
# backend knows differs, so the corroborating evidence differs with it. Keyed on the MPU
# BACKEND, not on the arch, because armv7m does not imply the ARM MemManage unit:
#   armv7m   hardware stacking aborts before the handler runs, so no frame was written at
#            all. Through the ARM MemManage unit that is CFSR MSTKERR (xmc4800-relax,
#            CFSR=0x92). Through a BUS-level slave-port unit it is BusFault STKERR with
#            IMPRECISERR and MSTKERR CLEAR (frdmk64f/SYSMPU, CFSR=0x1400), so the second
#            shape needs the SYSMPU line naming the denied write to tell it from a bare
#            BusFault.
#   armv6m   no CFSR and no MMFAR exist, so this arm is WEAKER by hardware and not by
#            omission: the frame's stack (PSP) is all there is to key on. The overflow
#            frame is garbage by construction, so nothing here asserts a plausible PC.
#   rv32imac the trap prologue is software and runs M-mode, which bypasses the unlocked
#            PMP entries, so the frame IS written - below the thread's stack. Nothing
#            latches; the store access fault on the recursion's own push is the tell, and
#            the stack-bounds test is the only thing that refuses the frame.
#   rxv3     the same shape as rv32imac. Supervisor bypasses the RX MPU, and RXv3 CANCELS
#            the faulting instruction and restores SP, so no SP-based test can see the
#            overflow at all - kickos_fault_below_stack is what refuses it.
#
# FS_CAPTURE=<log> judges a CAPTURED log instead of booting an image, and exists because
# armv6m and rxv3 have no runner to boot on: the refusal arms need KICKOS_HAVE_MPU and the
# one armv6m QEMU board has none, while rxv3 has neither an emulator nor a CI gate. Their
# clauses would otherwise ship never having executed. A log carries no exit status, so the
# exit clauses announce themselves NOT EVALUATED rather than silently passing.
#
# `offstack' (faultsurvive_off, KICKOS_FS_MODE 2): the worker points SP at a buffer
# outside its stack and faults there. The frame is complete and the thread really is
# unprivileged in thread mode, so every register-derived clause says yes and NO status
# bit is set on any backend: the stack-bounds test alone can refuse it. Delete that test
# and this arm reports a thread kill instead of a panic, which is what makes it the
# witness that the backend calls kickos_fault_frame_trusted at all.
#
# `lowedge' (faultsurvive_lowedge, KICKOS_FS_MODE 4): the half a FRAME-only bound misses.
# The worker runs on a caller-owned stack and parks its sp inside that stack with room for
# the frame but not for the kernel's C dispatch beneath it. A frame-only bound therefore
# says yes, the frame lands in bounds, the thread is killed cleanly, and the reporter chain
# runs privileged through the app's poisoned band below stack_lo. So this arm has a POSITIVE
# tell as well as the shared negative one: root, which only runs at all when the fault was
# survivable, prints the band's corruption. The extent bound refuses the sp before the first
# store, so the fixed system panics and root never runs.
#
# `misalign' (faultsurvive_misalign, KICKOS_FS_MODE 5): the worker drops sp two bytes, still
# deep inside its own stack and in bounds, so bounds and extent both pass and only alignment
# can refuse it.
# WHY THIS ARM CANNOT WITNESS THE LIVE-LOCK IT PREVENTS. What a misaligned sp costs is
# per-core. On a core that TRAPS a misaligned store, the frame's first store faults, the
# nested trap re-enters the prologue, rebuilds 128 bytes lower, faults again, and descends
# forever with no watchdog and no write ever landing. QEMU virt COMPLETES misaligned stores,
# so it cannot produce that at all: pre-fix it writes the whole frame in bounds and kills the
# thread cleanly. This arm therefore proves the REFUSAL and not the live-lock. The ESP32-C6
# is the exposed core.
#
# Both refusal arms share the negative claim: no kill banner, a panic dump, exit 132.
#
# QEMU, machine from kickos_add_qemu_test; the survive arm also runs natively on the sim.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: [FS_CAPTURE=<log>] check_faultsurvive.sh <elf|-> <arm: survive|overflow|offstack|kwrite|lowedge|misalign> [arch]"
elf="${1:?$_usage}"
arm="${2:?$_usage}"
# Passed, not sniffed out of the dump: which fact corroborates a refusal is a property of
# the backend, and a gate that read it back from the output it is judging would accept
# whichever dump it got.
arch="${3:-armv7m}"

# FS_CAPTURE judges a CAPTURED log instead of booting an image, because two of the fleet's
# enforcement classes have no runner to boot on: armv6m registers no faultsurvive arm (the
# refusal arms need KICKOS_HAVE_MPU, and the one armv6m QEMU board has none) and rxv3 has no
# emulator and no CI gate at all. Without this the clauses below could be written for those
# two and never once execute, which is the same unfalsifiable shape the gate exists to refuse.
# `-` for the elf, so a call site cannot read as having run an image when it did not.
CAPTURED=0
if [ -n "${FS_CAPTURE:-}" ]; then
    CAPTURED=1
    [ "$elf" = "-" ] || fail "FS_CAPTURE is set, so no image is booted: pass - for the elf"
    [ -s "$FS_CAPTURE" ] || fail "FS_CAPTURE=$FS_CAPTURE is missing or empty"
    # Same CR strip run_image does: a KICKOS_CONSOLE_CRLF board emits CR, and every silicon
    # capture is CRLF throughout.
    OUT="$(tr -d '\r' < "$FS_CAPTURE")"
    # RC is the one thing a log cannot carry. Left UNSET rather than defaulted, so the exit
    # clauses below have to opt out by name instead of silently comparing against a zero.
    printf '%s\n' "$OUT"
else
    [ "$elf" != "-" ] || fail "no FS_CAPTURE, so an elf is required"
    run_image "$elf"
fi

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
        if [ "$CAPTURED" -eq 0 ]; then
            if [ "$RC" -ne 0 ]; then
                fail "expected a clean exit 0 once root returned, got $RC"
            fi
        else
            echo "NOT EVALUATED: the clean exit 0. A capture carries no exit status." >&2
        fi
        echo "PASS: 'faulter' died at line $killed and root ran at line $survived"
        ;;
    overflow | offstack | kwrite | lowedge | misalign)
        # Checked FIRST, ahead of the shared negative clauses: on lowedge the pre-fix system
        # kills the thread cleanly and only this line reports the privileged writes that went
        # under stack_lo while it did. Reporting the missing panic instead would name the
        # symptom and bury the defect.
        if [ "$arm" = lowedge ] && has "lowband] CORRUPTED"; then
            fail "lowedge: the kernel's trap dispatch ran below stack_lo through a U-mode sp"
        fi
        if has_e "$(thread_fault_re faulter)"; then
            fail "$arm: the fault was redirected to the exit stub instead of escalating"
        fi
        if ! has_e "$KOS_PANIC_RE"; then
            fail "$arm: the fault reached no dump (looped, or walked into a neighbour?)"
        fi
        # The kwrite arm is the trap-stack security regression: the worker aimed its SP at a
        # kernel word. A backend that stored the frame through the U-mode SP prints this from
        # its panic path; a fixed one refuses the SP first and leaves the word intact, so the
        # line MUST be absent. The panic above already proves the SP was rejected, not run on.
        if [ "$arm" = kwrite ] && has "trapwitness] CORRUPTED"; then
            fail "kwrite: the trap prologue stored through the U-mode SP into kernel memory"
        fi
        # Corroboration, so the arm cannot pass on ANY panic that happens to occur.
        why=""
        case "$arch:$arm" in
            armv7m:overflow)
                # TWO stacking-abort shapes, because armv7m does not imply the ARM MemManage
                # unit. xmc4800-relax faults through it and latches MSTKERR (CFSR=0x92). The
                # K64F's SYSMPU is a BUS-level slave-port unit, so the same stacking abort
                # comes back as BusFault STKERR with IMPRECISERR, CFSR=0x1400 and MSTKERR clear,
                # so requiring MSTKERR alone failed a correct capture. The SYSMPU line is
                # what keeps the second shape from accepting a bare BusFault: it names the
                # denied write, and only a SYSMPU board can print it.
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || fail "the dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$cfsr & 0x10 )) -ne 0 ]; then
                    why="CFSR=0x$cfsr, MSTKERR"
                elif [ $(( 0x$cfsr & 0x1000 )) -ne 0 ] && has "SYSMPU ISOLATION FAULT"; then
                    why="CFSR=0x$cfsr, BusFault STKERR corroborated by SYSMPU ISOLATION FAULT"
                else
                    fail "CFSR=0x$cfsr is neither MSTKERR nor a SYSMPU-corroborated STKERR: this was not a stacking failure"
                fi
                ;;
            armv6m:overflow | armv6m:offstack)
                # armv6m has NO CFSR and no MMFAR, so there is no status bit to key on and
                # this arm is WEAKER than its armv7m twin by hardware, not by omission. What
                # is left is real: the reporter names the stack the frame came from, and a
                # user thread's frame is on PSP. An MSP frame would mean the fault was taken
                # in kernel context and the arm would prove nothing about a user thread.
                # Requiring the CFSR to be ABSENT is the positive control: it refuses an
                # armv7m capture handed to this arm by mistake.
                if has "CFSR="; then
                    fail "this dump carries a CFSR, so it is not an armv6m capture"
                fi
                has "(PSP)" || fail "the dump does not name PSP: the frame was not taken from a thread stack"
                why="frame on PSP, no CFSR to latch (armv6m has none)"
                # The overflow frame is garbage by construction, the hardware stacking writing
                # into the region that overflowed, so nothing here asserts a plausible PC.
                # A PC of 0xffffffff with a zero xPSR is the signature, not a capture defect;
                # asserting it exactly would assert what the ISA cannot promise.
                ;;
            rxv3:overflow)
                # Same shape as rv32imac: the RX MPU denies the recursion's own push and the
                # report credits it to the thread. Supervisor bypasses the RX MPU, so the
                # frame itself is not what refuses this. kickos_fault_below_stack is.
                if ! has "MPU FAULT: thread 'faulter' attempted write"; then
                    fail "no denied write credited to 'faulter': the recursion never ran off its granted stack"
                fi
                why="RX-MPU-denied write by 'faulter'"
                ;;
            rxv3:offstack)
                has "RX EXCEPTION (privileged instruction)" \
                  || fail "the dump names a cause other than the deliberate privileged instruction"
                # PSW.PM (bit 20) is the privilege clause reporting that it said YES. RXv3
                # CANCELS the faulting instruction and restores SP, so no SP-based test can see
                # the overflow and the bounds test is the only thing left that can refuse this.
                psw="$(printf '%s\n' "$OUT" | sed -n 's/.*PSW=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$psw" ] || fail "the dump carries no PSW (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$psw & 0x100000 )) -eq 0 ]; then
                    fail "PSW=0x$psw has PM clear: the fault was not taken in user mode, so the privilege clause refused it and the arm proves nothing about the bounds test"
                fi
                why="PSW=0x$psw, PM=1 (user)"
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
                if ! has "MPU FAULT: thread 'faulter' attempted write"; then
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
            rv32imac:lowedge | rv32imac:misalign)
                # Same trap and the same two facts as kwrite: the panic taken in user mode
                # is the prologue refusing this sp before it stored anything, and neither arm
                # claims more than that here. lowedge's band tells the two directions apart
                # only when it is DIRTY: a refusal ends the system before root runs, so its
                # silence is the panic's doing and not an observation. misalign has nothing
                # further to show, since QEMU virt completes the misaligned stores a trapping
                # core would loop on.
                has "RISC-V TRAP (illegal instruction)" \
                  || fail "the dump names a cause other than the deliberate illegal instruction"
                mst="$(printf '%s\n' "$OUT" | sed -n 's/.*mstatus=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$mst" ] || fail "the dump carries no mstatus (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$mst & 0x1800 )) -ne 0 ]; then
                    fail "mstatus=0x$mst has MPP != U: the sp the guard refused was not a thread's"
                fi
                why="mstatus=0x$mst, MPP=U"
                ;;
            rv32imac:kwrite)
                # Same trap as offstack, aimed at a kernel word. The panic taken in user mode
                # (MPP=U) is the trap prologue refusing the out-of-bounds SP before it stored,
                # and the absent trapwitness line (checked above) is the write not landing.
                has "RISC-V TRAP (illegal instruction)" \
                  || fail "the dump names a cause other than the deliberate illegal instruction"
                mst="$(printf '%s\n' "$OUT" | sed -n 's/.*mstatus=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$mst" ] || fail "the dump carries no mstatus (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$mst & 0x1800 )) -ne 0 ]; then
                    fail "mstatus=0x$mst has MPP != U: the wild SP was not rejected in user mode"
                fi
                why="mstatus=0x$mst, MPP=U, witness intact"
                ;;
            rxv3:misalign)
                # Same entry as rxv3:kwrite (int #1), aimed at an in-bounds MISALIGNED USP,
                # so only the alignment leg can refuse it. A fault-path entry would reach
                # kickos_fault_frame_trusted instead, which tests range and extent and not
                # alignment, and would report a clean thread kill.
                has "RX EXCEPTION (wild stack)" \
                  || fail "the dump names a cause other than the rejected misaligned USP: the syscall trap's alignment leg did not refuse it"
                usp="$(printf '%s\n' "$OUT" | sed -n 's/.*USP=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$usp" ] || fail "the wild-stack dump carries no USP"
                # The leg tests `and #3`, so the discriminator is the low TWO bits, not
                # evenness: sub #2 from a 4-aligned sp lands at 2 mod 4, which is even.
                case "$usp" in
                    *[048cC]) fail "USP=0x$usp is 4-byte aligned: this is not the misaligned case" ;;
                    *) ;;
                esac
                why="RX alignment refusal USP=0x$usp"
                ;;
            rxv3:kwrite)
                # The worker entered kickos_rx_syscall_trap (int #1) with a wild USP. The
                # fix rejects it before the store and panics through kickos_rx_bad_usp; the
                # absent trapwitness line (checked above) is the USP write not landing.
                has "RX EXCEPTION (wild stack)" \
                  || fail "the dump names a cause other than the rejected wild USP: the syscall trap did not refuse it"
                usp="$(printf '%s\n' "$OUT" | sed -n 's/.*USP=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$usp" ] || fail "the wild-stack dump carries no USP"
                why="RX wild-stack refusal USP=0x$usp, witness intact"
                ;;
            *)
                fail "$arm: no corroborating evidence is defined for arch '$arch'"
                ;;
        esac
        # Which dead-end ran: the thread-naming reporter ends the system with 0, the shared
        # kfault_terminate with 132. Both are the unchanged pre-isolation behaviour for
        # their backend, so the arm pins whichever one this fault reached.
        want=132
        if has "MPU FAULT: thread"; then
            want=0
        fi
        if [ "$CAPTURED" -eq 0 ]; then
            if [ "$RC" -ne "$want" ]; then
                fail "expected exit $want from the escalation path, got $RC"
            fi
            echo "PASS: the $arm fault escalated to the panic dump ($why, exit $RC)"
        else
            echo "NOT EVALUATED: which dead-end ran (expected exit $want). A capture carries no exit status." >&2
            echo "PASS: the $arm fault escalated to the panic dump ($why, from a capture)"
        fi
        ;;
    *)
        fail "$_usage"
        ;;
esac
exit 0
