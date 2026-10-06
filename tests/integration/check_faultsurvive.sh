#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-isolation witness. One arm per image: survive and lowedge keep the system up, the four
# refusal arms turn on what a REFUSED sp costs, which <outcome> carries.
#
# THE FOUR REFUSAL ARMS ARE NOT ALL ONE CLAIM ANY MORE. Where the sp is refused by the TRAP
# ENTRY, that entry now contains the offending thread and the system runs on (`contained`);
# where it is refused later, by the fault path's frame-validity test on a frame that was
# already written, nothing can be contained and the system still ends (`terminated`). Which of
# the two a backend takes is a property of where its refusal lives, so the arch clauses below
# read under both: on rv32imac all four arms enter through trap_entry, while on the ARM
# backends the hardware or the MPU faults first and the switcher guard never sees the sp.
#
# `survive' (faultsurvive, KICKOS_FS_MODE 0): an unprivileged worker executes an
# undefined instruction and only that thread dies. The claim is ORDERING, not presence:
# both lines appearing proves nothing, since a system that panicked after printing them
# would look the same. Root is parked in join() until the worker is gone, so the
# survivor's line coming after the kill banner is causal.
#
# `overflow' (faultsurvive_ovf, KICKOS_FS_MODE 1): the worker recurses off its stack, and
# the fault must reach the PANIC dump (design 4.2) rather than the thread kill. Keyed on
# the MPU BACKEND, since armv7m does not imply the ARM MemManage unit:
#   armv7m   hardware stacking aborts before the handler runs, so no frame is written.
#            Through the ARM MemManage unit that is CFSR MSTKERR (xmc4800-relax,
#            CFSR=0x92). Through a BUS-level slave-port unit it is BusFault STKERR with
#            IMPRECISERR and MSTKERR CLEAR (frdmk64f/SYSMPU, CFSR=0x1400), so the second
#            shape needs the SYSMPU line naming the denied write to tell it from a bare
#            BusFault.
#   armv6m   keys on the frame's stack (PSP), which is what this hardware latches; an
#            absent CFSR is the positive control that the capture is an armv6m one.
#   rv32imac nothing latches: the trap entry is software, so the store access fault on the
#            recursion's own push is the tell. The sp it interrupts is far below stack_lo, so
#            the entry refuses it, leaves the frame on the per-hart trap stack, and the
#            frame-on-the-kernel-stack test is what refuses that frame.
#   rxv3     the same shape as rv32imac. Supervisor bypasses the RX MPU, and RXv3 CANCELS
#            the faulting instruction and restores SP, so kickos_fault_below_stack is what
#            refuses it and no SP-based test can see the overflow.
#
# `offstack' (faultsurvive_off, KICKOS_FS_MODE 2): the worker points SP at a buffer
# outside its stack and faults there. The frame is complete and the thread really is
# unprivileged in thread mode, so every register-derived clause says yes and NO status
# bit is set on any backend: the stack-bounds test alone can refuse it. Delete that test
# and this arm reports a thread kill instead of a panic, which is what makes it the
# witness that the backend calls kickos_fault_frame_trusted at all, or on rv32imac its
# kernel-stack twin, the entry having moved off the interrupted thread stack.
#
# `lowedge' (faultsurvive_lowedge, KICKOS_FS_MODE 4): the LOW EDGE, and the one arm here that
# expects a KILL. The worker runs on a caller-owned stack and parks its sp inside that stack
# with less room beneath it than the frame plus the kernel's C dispatch would need. On
# rv32imac that sp is legal: the entry transfers to the thread's own kernel stack, so the
# frame and every byte of C below it land there and nothing privileged is written under the
# parked sp. So the claim is the STRONGER one, a clean thread kill AND the app's poisoned band
# below stack_lo intact, which main reads back and prints either way. An entry that adopted
# the sp instead would run the reporter chain privileged through that band, and the band says
# so; an entry that refused the sp would panic and main would never run.
#
# `misalign' (faultsurvive_misalign, KICKOS_FS_MODE 5): the worker drops sp two bytes, still
# deep inside its own stack and in bounds, so bounds and extent both pass and only alignment
# can refuse it. WHAT THIS ARM WITNESSES IS THE REFUSAL. The cost a misaligned sp carries is
# per-core: a core that TRAPS a misaligned store descends forever, the frame's first store
# faulting, the nested trap rebuilding 128 bytes lower and faulting again, with no watchdog
# and no write ever landing. QEMU virt COMPLETES misaligned stores, so pre-fix it writes the
# whole frame in bounds and kills the thread cleanly. The ESP32-C6 is the exposed core.
#
# `unread' (faultsurvive_unread, KICKOS_FS_MODE 6, armv7m on QEMU): the worker points SP at an
# address no memory answers and traps. Its stacking aborts, and a privileged read of that frame
# would fault again in handler mode, where a fault escalated to HardFault locks the core up. The
# claim is ONE dump, which names the stacking abort and says it read no frame.
#
# The refusal arms share the negative claim of no kill banner. A contained refusal leaves the
# system running; a terminated one ends in a panic dump and exit 132.
#
# QEMU, machine from kickos_add_qemu_test; the survive arm also runs natively on the sim. With
# KOS_CAPTURE it reads a silicon capture, which carries no exit status.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_faultsurvive.sh <elf> <arm: survive|overflow|offstack|kwrite|lowedge|misalign|unread> <arch> <outcome: contained|terminated>
       KOS_CAPTURE=<log> check_faultsurvive.sh <board-build> <kickos-source> <cmake> <arm> <arch> <outcome>"
if judging_capture; then
    shift 3
    judge_capture faultsurvive
else
    elf="${1:?$_usage}"
    shift
    run_image "$elf"
fi
arm="${1:?$_usage}"
arch="${2:?$_usage}"
outcome="${3:?$_usage}"

if has "\[fs\] ERROR"; then
    cfail error "the app reported its own failure"
fi
# First matching line number, so the two arms can be ordered against each other.
line_of() { printf '%s\n' "$OUT" | grep -nE "$1" | head -n1 | cut -d: -f1; }

about="$(line_of "\[fs\] worker about to fault")"
if [ -z "$about" ]; then
    cfail reached "the worker never reached its deliberate fault"
fi
# <line> <what>: the record at <line> follows the worker's announcement, so it is this fault's.
after_about() {
    if [ "$1" -le "$about" ]; then
        cfail order "$2 at line $1 is not after the worker's announcement at line $about"
    fi
}

# The banner the backend's own trap entry prints when it refuses an sp. Carried per arch and
# not matched loosely, so a refusal that came out of another backend's reporter fails here.
# THE NOUN CARRIES THE OUTCOME, and matching it is what keeps the two apart on the wire: a
# contained refusal spells CONTAINED precisely so it does NOT match tests/lib/panic.ere, which
# every "=== <ARCH> EXCEPTION" and "=== RISC-V TRAP" does.
wild_refusal_re() { # <arch> <outcome>
    _n=CONTAINED
    if [ "$2" = terminated ]; then
        _n=EXCEPTION
    fi
    case "$1" in
        rv32imac)
            if [ "$2" = terminated ]; then
                printf '%s' "=== RISC-V TRAP \\(wild stack\\)"
            else
                printf '%s' "=== RISC-V CONTAINED \\(wild stack\\)"
            fi
            ;;
        rxv3)     printf '%s' "=== RX $_n \\(wild stack\\)" ;;
        armv7m)   printf '%s' "=== ARMV7M $_n \\(wild PSP" ;;
        armv6m)   printf '%s' "=== ARMV6M $_n \\(wild PSP" ;;
        *)        fail "no wild-stack refusal banner is defined for arch '$1'" ;;
    esac
}

case "$arm" in
    survive | lowedge)
        killed="$(line_of "$(thread_fault_re faulter)")"
        if [ -z "$killed" ]; then
            cfail killed "no thread-kill for 'faulter' (the fault ended the system?)"
        fi
        after_about "$killed" "the kill"
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            cfail survived "main never ran again after the worker faulted"
        fi
        if [ "$survived" -le "$killed" ]; then
            cfail order "main's line is at $survived, not after the kill at $killed"
        fi
        assert_no_panic "the worker was killed AND the system panicked"
        # The band, and BOTH directions are clauses. Corrupted names the privileged writes
        # that went under the parked sp; a missing verdict line means main reached the readback
        # and printed neither, which is the silent arm this pair exists to refuse.
        if [ "$arm" = lowedge ]; then
            if has "lowband] CORRUPTED"; then
                cfail lowband "lowedge: the kernel's trap dispatch ran below stack_lo through a U-mode sp"
            fi
            intact="$(line_of "\[fs\] \[lowband\] INTACT")"
            if [ -z "$intact" ]; then
                cfail lowband "lowedge: main printed no band verdict, so nothing here witnessed the
    band at all: the readback is compiled out, or this is not the mode 4 image"
            fi
            if [ "$intact" -le "$killed" ]; then
                cfail order "lowedge: the band verdict at $intact is not after the kill at $killed,
    so it was read before the fault it is meant to judge"
            fi
        fi
        status_clause "the system exited 0 once main returned" 0
        echo "PASS: 'faulter' died at line $killed and main ran at line $survived"
        ;;
    overflow | offstack | kwrite | misalign | unread)
        if has_e "$(thread_fault_re faulter)"; then
            cfail redirected "$arm: the fault was redirected to the exit stub instead of escalating"
        fi
        # kwrite is the trap-stack security regression: the worker aimed its SP at a kernel
        # word. A backend that stored the frame through the U-mode SP prints this from its
        # panic path, so a fixed one leaves the word intact and the line absent. Claimed under
        # BOTH outcomes: it is about what the prologue wrote, not about what it did next.
        if [ "$arm" = kwrite ] && has "trapwitness] CORRUPTED"; then
            cfail kwrite "kwrite: the trap prologue stored through the U-mode SP into kernel memory"
        fi
        if [ "$outcome" = contained ]; then
            # assert_no_panic IS available on this path now: a contained refusal spells its
            # banner CONTAINED, which check_panic_banners keeps OUT of tests/lib/panic.ere
            # precisely so the two outcomes stay apart. Without it a terminal panic AFTER the
            # survivor line satisfied every clause below.
            assert_no_panic "$arm: something panicked on a run that was supposed to contain"
            refused="$(line_of "$(wild_refusal_re "$arch" "$outcome")")"
            if [ -z "$refused" ]; then
                cfail refusal "$arm: no $arch wild-stack refusal reached the wire, so the sp was
    ACCEPTED and this arm witnessed nothing"
            fi
            after_about "$refused" "the refusal"
            survived="$(line_of "\[fs\] survivor ran after the fault")"
            if [ -z "$survived" ]; then
                cfail survived "$arm: the refusal ended the system, so the whole image paid for one
    thread's sp"
            fi
            if [ "$survived" -le "$refused" ]; then
                cfail order "$arm: main's line is at $survived, not after the refusal at $refused"
            fi
            status_clause "the system exited 0 once main outlived the refusal" 0
            # The rejected USP, read off the refusal record: kickos_rx_bad_usp's guard has an
            # alignment leg and a bounds leg, and the USP's low two bits say which one refused.
            if [ "$arch" = rxv3 ]; then
                usp="$(printf '%s\n' "$OUT" | sed -n "$refused,\$p" \
                    | sed -n 's/.*USP=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$usp" ] || cfail usp "$arm: the refusal record carries no USP"
                case "$arm:$usp" in
                    misalign:*[048cC])
                        cfail usp "USP=0x$usp is 4-byte aligned, so the alignment leg did not
    refuse it"
                        ;;
                    kwrite:*[048cC]) ;;
                    kwrite:*)
                        cfail usp "USP=0x$usp is misaligned, so the alignment leg refused it, not the
    bounds leg"
                        ;;
                    *) ;;
                esac
            fi
            # ATTRIBUTION, and rv32imac is the arch that needs the clause: its trap entry
            # catches a thread that overflowed its own stack BEFORE the PMP-denial report that
            # used to credit the write, so without a name in the refusal itself an overflow
            # dies anonymously. No other backend prints one here.
            if [ "$arch" = rv32imac ]; then
                # BOUND TO THE REFUSAL RECORD, and SEARCHED FROM IT. A bare match is
                # satisfied by any earlier line naming the thread, including its own spawn
                # trace; taking the first match overall and then comparing rejects a run whose
                # attribution is present but preceded by one, so the scan starts at the
                # refusal instead.
                _credited="$(printf '%s\n' "$OUT" \
                    | awk -v start="$refused" 'NR >= start && /thread '"'"'faulter'"'"'/ { print NR; exit }')"
                if [ -z "$_credited" ]; then
                    cfail attribution "$arm: no line at or after the refusal at $refused credits 'faulter',
    so the containment took the attribution the panic path used to carry"
                fi
            fi
            echo "PASS: $arm refused at line $refused and main ran at line $survived"
            exit 0
        fi
        if [ "$outcome" != terminated ]; then
            fail "unknown outcome '$outcome': expected 'contained' or 'terminated'"
        fi
        dumped="$(line_of "$KOS_PANIC_RE")"
        if [ -z "$dumped" ]; then
            cfail dump "$arm: the fault reached no dump (looped, or walked into a neighbour?)"
        fi
        after_about "$dumped" "the dump"
        # Corroboration, so the arm cannot pass on ANY panic that happens to occur.
        why=""
        case "$arch:$arm" in
            armv7m:overflow)
                # TWO stacking-abort shapes. xmc4800-relax faults through the ARM MemManage
                # unit and latches MSTKERR (CFSR=0x92). The K64F's SYSMPU is a BUS-level
                # slave-port unit, so the same abort comes back as BusFault STKERR with
                # IMPRECISERR, CFSR=0x1400, MSTKERR clear. The SYSMPU line names the denied
                # write and only a SYSMPU board prints it, which is what keeps the second
                # shape from accepting a bare BusFault.
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || cfail cause "the dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$cfsr & 0x10 )) -ne 0 ]; then
                    why="CFSR=0x$cfsr, MSTKERR"
                elif [ $(( 0x$cfsr & 0x1000 )) -ne 0 ] && has "SYSMPU ISOLATION FAULT"; then
                    why="CFSR=0x$cfsr, BusFault STKERR corroborated by SYSMPU ISOLATION FAULT"
                else
                    cfail cause "CFSR=0x$cfsr is neither MSTKERR nor a SYSMPU-corroborated STKERR: this was not a stacking failure"
                fi
                ;;
            armv6m:overflow | armv6m:offstack)
                # This hardware keys on the stack the reporter names: a user thread's frame
                # is on PSP, so an MSP frame would mean a fault taken in kernel context and
                # would prove nothing about a user thread. Requiring the CFSR to be ABSENT is
                # the positive control that refuses an armv7m capture handed here by mistake.
                if has "CFSR="; then
                    cfail cause "this dump carries a CFSR, so it is not an armv6m capture"
                fi
                has "(PSP)" || cfail cause "the dump does not name PSP: the frame was not taken from a thread stack"
                why="frame on PSP, no CFSR to latch (armv6m has none)"
                # The overflow frame is garbage by construction, the hardware stacking into
                # the region that overflowed, so no clause here asserts a plausible PC. A PC
                # of 0xffffffff with a zero xPSR is the signature and the ISA promises no
                # exact value.
                ;;
            rxv3:overflow)
                # The RX MPU denies the recursion's own push and the report credits it to the
                # thread. Supervisor bypasses that MPU, so kickos_fault_below_stack is what
                # refuses this and not the frame's own store.
                if ! has "MPU FAULT: thread 'faulter' attempted write"; then
                    cfail cause "no denied write credited to 'faulter': the recursion never ran off its granted stack"
                fi
                why="RX-MPU-denied write by 'faulter'"
                ;;
            rxv3:offstack)
                has "RX EXCEPTION (privileged instruction)" \
                  || cfail cause "the dump names a cause other than the deliberate privileged instruction"
                # PSW.PM (bit 20) is the privilege clause reporting that it said YES. RXv3
                # CANCELS the faulting instruction and restores SP, so the bounds test is the
                # only thing left that can refuse this.
                psw="$(printf '%s\n' "$OUT" | sed -n 's/.*PSW=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$psw" ] || cfail cause "the dump carries no PSW (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$psw & 0x100000 )) -eq 0 ]; then
                    cfail cause "PSW=0x$psw has PM clear: the fault was not taken in user mode, so the privilege clause refused it and the arm proves nothing about the bounds test"
                fi
                why="PSW=0x$psw, PM=1 (user)"
                ;;
            armv7m:unread)
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || cfail cause "the dump carries no CFSR: the report died before it, the core locked up reading the frame?"
                if [ $(( 0x$cfsr & 0x1818 )) -eq 0 ]; then
                    cfail cause "CFSR=0x$cfsr carries no stacking-abort bit, so the frame was readable and this arm witnessed nothing"
                fi
                has "frame not read: its stacking at" \
                  || cfail frame "the dump does not say it left the aborted frame unread"
                banners="$(printf '%s\n' "$OUT" | grep -oE "$KOS_PANIC_RE" | wc -l | tr -d ' ')"
                if [ "$banners" -ne 1 ]; then
                    cfail doubled "$banners dump banners: the report faulted again"
                fi
                why="CFSR=0x$cfsr, stacking abort, frame left unread"
                ;;
            armv7m:offstack)
                # The frame WAS written, so the bits the CFSR early-out would refuse on are
                # exactly the ones that must be clear. Without this clause the arm passes on
                # a stacking abort and says nothing about the bounds test.
                cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                [ -n "$cfsr" ] || cfail cause "the dump carries no CFSR (KICKOS_PANIC_DUMP off?)"
                if [ $(( 0x$cfsr & 0x1818 )) -ne 0 ]; then
                    cfail cause "CFSR=0x$cfsr carries a stacking-abort bit, so the CFSR early-out could have refused this frame and the arm proves nothing about the bounds test"
                fi
                why="CFSR=0x$cfsr, no stacking-abort bit"
                ;;
            *)
                fail "$arm: no corroborating evidence is defined for arch '$arch'"
                ;;
        esac
        # Which dead-end ran: the thread-naming reporter ends the system with 0, the shared
        # kfault_terminate with 132. The arm pins whichever one this backend reaches.
        want=132
        if has "MPU FAULT: thread"; then
            want=0
        fi
        status_clause "the escalation ended the system with exit $want" "$want"
        if ! judging_capture; then
            echo "PASS: the $arm fault escalated to the panic dump ($why, exit $want)"
        else
            echo "PASS: the $arm fault escalated to the panic dump ($why, from a capture)"
        fi
        ;;
    *)
        fail "$_usage"
        ;;
esac
exit 0
