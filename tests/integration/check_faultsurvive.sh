#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Fault-isolation witness. One arm per image: survive and lowedge keep the system up, and the
# refusal arms turn on what a REFUSED sp costs, which <outcome> carries.
#
# Where the TRAP ENTRY refuses the sp, it contains the offending thread and the system runs on
# (`contained`). Where the fault path meets it instead, the kill rests on registers alone and
# reads nothing of the frame, so the thread's task dies and the system runs on (`killed`). On
# rv32imac all four arms enter through trap_entry; on the ARM backends the hardware or the MPU
# faults first and the switcher guard never sees the sp.
#
# `survive' (faultsurvive, KICKOS_FS_MODE 0): an unprivileged worker executes an
# undefined instruction and only that thread dies. The claim is ORDERING, not presence:
# both lines appearing proves nothing, since a system that panicked after printing them
# would look the same. Root is parked in join() until the worker is gone, so the
# survivor's line coming after the kill banner is causal.
#
# `overflow' (faultsurvive_ovf, KICKOS_FS_MODE 1): the worker recurses off its stack and its
# task alone dies. Keyed on the MPU BACKEND, since armv7m does not imply the ARM MemManage unit:
#   armv7m   hardware stacking aborts before the handler runs, so no frame is written and the
#            kill record names no PC. Through the ARM MemManage unit the abort is CFSR MSTKERR
#            (xmc4800-relax, CFSR=0x92); through a BUS-level slave-port unit it is BusFault
#            STKERR with MSTKERR CLEAR (frdmk64f/SYSMPU, CFSR=0x1400), which the record's
#            SYSMPU ISOLATION FAULT line tells from a bare BusFault.
#   armv6m   nothing latches, so the kill record is a bare `PC unknown'; an absent CFSR is
#            the positive control that the capture is an armv6m one.
#   rv32imac nothing latches: the trap entry is software, so the store access fault on the
#            recursion's own push is the tell. The sp it interrupts is far below stack_lo, so
#            the entry refuses it, leaves the frame on the per-hart trap stack, and the
#            frame-on-the-kernel-stack test is what refuses that frame.
#   rxv3     `killed': the frame is on the ISP and the exit stub runs on the thread's kernel
#            block, so nothing privileged touches the exhausted USP and the worker dies alone.
#            The kill record's MPESTS names a denied operand write.
#
# `offstack' (faultsurvive_off, KICKOS_FS_MODE 2): the worker points SP at a buffer
# outside its stack and faults there. The frame is complete and the thread really is
# unprivileged in thread mode, so NO status bit is set on any backend and the stack-bounds
# test alone can refuse the frame. On ARM the refusal costs the record its PC: the kill
# record says `PC unknown' with no stacking-abort bit, and deleting the test makes it name the
# PC the task wrote, which is what makes this arm the witness that the backend calls
# kickos_fault_frame_trusted at all. On rv32imac it is the kernel-stack twin, the entry having
# moved off the interrupted thread stack. On rxv3 no frame lies at the USP and the stub never
# runs on it, so the arm is `killed', on the deliberate privileged instruction's cause.
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
# claim is a kill whose record names the stacking abort and no PC, and no dump.
#
# `fpunread' (faultsurvive_fpunread, KICKOS_FS_MODE 18, armv7m with an FPU on QEMU): unread with FP
# state live, so the refused entry also leaves a lazy FP save pending at FPCAR, where no memory
# answers. The handler drops it before any FP instruction runs, or the save faults inside it and
# the system ends. The claim is unread's.
#
# A contained refusal leaves the system running with no kill banner; a killed arm names the
# worker in a kill record and main runs after it.
#
# `wildsvc' (faultsurvive_wildsvc, KICKOS_FS_MODE 14, armv7m and armv6m silicon): the worker points SP
# just above a word of main's and executes `svc`. The MPU refuses the entry's stacking, so the
# fault is taken and the SVCall left pending; the claim is overflow's kill record naming the
# SVCall it dropped, main's word intact, and no SVCall run in the dead worker's place, whose guard
# would refuse the exit's stack as a CONTAINED wild PSP.
#
# `storesvc' (faultsurvive_storesvc, KICKOS_FS_MODE 15, frdmk64f): the worker stores to main's
# word and executes `svc` at once. The SYSMPU denies the store as an imprecise BusFault, and where
# that error meets the SVCall's entry the claim is wildsvc's, with IMPRECISERR in the record. Where
# it lands is the bus's timing, which the worker cannot force: one that lands before the svc pends
# no SVCall and fails here as witnessing nothing, never as a pass. wildsvc, which the same board
# builds, pends the SVCall on every run.
#
# `bxns' and `blxns' (faultsurvive_bxns and faultsurvive_blxns, KICKOS_FS_MODE 16 and 17, v8-M
# with the Security Extension): the worker branches to Non-secure state at its own code, which
# no privilege guards. The Non-secure fetch of a Secure address that holds no SG is refused (SFSR
# INVEP), the frame goes to a Non-secure stack, and the kill record must name the SFSR and no PC.
#
# `bxnsrom' (faultsurvive_bxnsrom, KICKOS_FS_MODE 19, rp2350 silicon): the worker branches to the
# boot ROM the IDAU leaves Exempt, which Non-secure state may fetch whatever the SAU says. Only the
# Non-secure MPU bring-up enables with no region stops it, at the first fetch: the kill record must
# name CFSR_NS with IACCVIOL and no PC.
#
# `div0' and `ldiv0' (faultsurvive_div0 and faultsurvive_ldiv0, KICKOS_FS_MODE 8 and 9): the
# worker divides by zero in 32 and in 64 bits. The outcome is the arch's: `killed' where the core
# or the run-time ABI traps it, the kill record naming the division as this arch's status says
# so, and `returned:<quotient>' where the ISA defines a quotient instead, `any' where it leaves
# the quotient undefined or no instruction of it divides at that width. Main runs after either.
#
# `swtrap' (faultsurvive_swtrap, KICKOS_FS_MODE 10, rxv3): the worker is spawned once per software
# trap, an INT #n or BRK through an INTB slot that is not the syscall's, brk last. INT is not
# privileged on RX, so each one reaches a first-level handler unless the entry tells a trap from an
# acceptance: every spawn must end in a kill naming the software-trap cause (0x100), alternating
# with its announcement, and the clock main reads around the loop must not have jumped.
#
# `forge' (faultsurvive_forge, KICKOS_FS_MODE 11, rxv3): the worker writes a PSW with PM clear
# wherever its task can, the staged syscall return (`head`), a thread's first frame (`first`) and
# a preempted thread's saved context (`preempt`), each against a thread of its own, and each target
# prints the mode it runs in afterwards. The outcome `user` is all three running unprivileged.
#
# `wildspin' (faultsurvive_wildspin, KICKOS_FS_MODE 13, rxv3): a thread of the faulter points its
# USP just above a word of main's and spins until preempted, so the SWINT switcher meets the USP
# with no syscall or fault before it. Judged as kwrite: a contained refusal by the bounds leg and
# main's word intact. Containment slays that thread alone, so the faulter must also report it
# gone between the refusal and main's line.
#
# `mppforge' (faultsurvive_mppforge, KICKOS_FS_MODE 12, rv32imac): the faulter plants MPP=M in
# every frame of a READY child it finds on a block it can write, then lets the child run. The
# child's first instruction reads mstatus, so the claim is that the child 'forged' dies from U-mode
# at that instruction, illegal-instruction cause, after the scan found no frame to plant, and never
# prints ESCALATED. <outcome> is `killed'.
#
# QEMU, machine from kickos_add_qemu_test; the survive and divide arms also run natively on the
# sim. With KOS_CAPTURE it reads a silicon capture, which carries no exit status.

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_faultsurvive.sh <elf> <arm: survive|overflow|stkof|offstack|kwrite|lowedge|misalign|unread|fpunread|wildsvc|storesvc|bxns|blxns|bxnsrom|div0|ldiv0|swtrap|forge|mppforge|wildspin> <arch> <outcome: contained|terminated|killed|user|returned:<quotient|any>>
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
if [ "$arm" = forge ]; then
    about="$(line_of "\[fs\] forger running")"
fi
if [ -z "$about" ]; then
    cfail reached "the worker never reached its deliberate fault"
fi
# The worker's kill record, its banner's indented lines.
kill_record() {
    printf '%s\n' "$OUT" | KOS_KP_BANNER="$(thread_fault_re faulter)" awk '
        on && !/^  / { exit }
        on { print }
        index($0, ENVIRON["KOS_KP_BANNER"]) > 0 { on = 1 }'
}

# The first PC line of the worker's kill record.
kill_pc_line() {
    printf '%s\n' "$OUT" | KOS_KP_BANNER="$(thread_fault_re faulter)" awk '
        on && /^=== / { exit }
        on && /^  PC/ { print; exit }
        index($0, ENVIRON["KOS_KP_BANNER"]) > 0 { on = 1 }'
}

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
wild_refusal_re() { # <arch>
    case "$1" in
        rv32imac) printf '%s' "=== RISC-V CONTAINED \\(wild stack\\)" ;;
        rxv3)     printf '%s' "=== RX CONTAINED \\(wild stack\\)" ;;
        armv7m)   printf '%s' "=== ARMV7M CONTAINED \\(wild PSP" ;;
        armv6m)   printf '%s' "=== ARMV6M CONTAINED \\(wild PSP" ;;
        *)        fail "no wild-stack refusal banner is defined for arch '$1'" ;;
    esac
}

case "$arm" in
    swtrap)
        [ "$arch:$outcome" = rxv3:killed ] || fail "swtrap: the only verdict this arm has is rxv3 'killed'"
        if has "\[fs\] \[clock\] JUMPED"; then
            cfail clock "swtrap: a software trap moved the clock"
        fi
        runs="$(printf '%s\n' "$OUT" | sed -n 's/.*\[fs\] \[clock\] STEADY: \([0-9][0-9]*\) traps.*/\1/p' | head -n1)"
        [ -n "$runs" ] || cfail clock "swtrap: main printed no steady clock verdict naming the trap count"
        # Each announcement, then its trap, then its kill, then the next announcement.
        kre="$(thread_fault_re faulter)"
        seq="$(printf '%s\n' "$OUT" | awk -v K="$kre" '
            /\[fs\] worker about to fault/ { printf "a"; next }
            /\[fs\] trap / { printf "t"; next }
            index($0, K) { printf "k"; next }
            /cause=0x100/ { printf "c"; next }
            /\[fs\] survivor ran after the fault/ { printf "s"; next }')"
        want="$(awk -v n="$runs" 'BEGIN { for (i = 0; i < n; i++) printf "atkc"; printf "s" }')"
        if [ "$seq" != "$want" ]; then
            cfail killed "swtrap: the events read '$seq', where $runs traps each killed with the
    software-trap cause and then main read '$want'"
        fi
        assert_no_panic "swtrap: a software trap ended the system"
        status_clause "the system exited 0 once main returned" 0
        echo "PASS: $runs software traps each killed 'faulter' alone and the clock held"
        ;;
    forge)
        [ "$arch:$outcome" = rxv3:user ] || fail "forge: the only verdict this arm has is rxv3 'user'"
        if has "\] SUPERVISOR"; then
            cfail forged "forge: a PSW the task wrote ran a thread supervisor"
        fi
        if has_e "$(thread_fault_re 'fs_[a-z]*')"; then
            cfail killed "forge: a thread of the forger was killed"
        fi
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            cfail survived "forge: main never ran again after the forger"
        fi
        for a in head first preempt; do
            v="$(line_of "\[fs\] \[$a\] USER")"
            if [ -z "$v" ]; then
                cfail "$a" "forge: the $a arm printed no verdict"
            fi
            after_about "$v" "the $a verdict"
            if [ "$survived" -le "$v" ]; then
                cfail order "forge: main's line is at $survived, not after the $a verdict at $v"
            fi
        done
        assert_no_panic "forge: the forger ended the system"
        status_clause "the system exited 0 once main returned" 0
        echo "PASS: each forged PSW left its thread unprivileged and main ran at line $survived"
        ;;
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
    mppforge)
        entry="$(printf '%s\n' "$OUT" \
            | sed -n 's/^\[fs\] \[forge\] child entry 0x\([0-9a-fA-F]*\)$/\1/p' | head -n1)"
        if [ -z "$entry" ]; then
            cfail entry "mppforge: the faulter announced no child, so none ran on its block"
        fi
        if has "\[fs\] \[forge\] ESCALATED"; then
            cfail escalated "mppforge: the child executed its first instruction in M-mode"
        fi
        if has "\[fs\] \[forge\] first frame on the child's user stack"; then
            cfail frame "mppforge: the child's first frame, mstatus included, lay where its task writes"
        fi
        verdict="$(line_of "\[fs\] \[forge\] no first frame on the child's user stack")"
        if [ -z "$verdict" ]; then
            cfail verdict "mppforge: the faulter printed no scan verdict"
        fi
        killed="$(line_of "$(thread_fault_re forged)")"
        if [ -z "$killed" ]; then
            cfail killed "mppforge: no thread-kill for the child 'forged'"
        fi
        if [ "$killed" -le "$verdict" ]; then
            cfail order "mppforge: the kill at $killed is not after the scan verdict at $verdict"
        fi
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            cfail survived "mppforge: main never ran again after the child died"
        fi
        if [ "$survived" -le "$killed" ]; then
            cfail order "mppforge: main's line is at $survived, not after the kill at $killed"
        fi
        record="$(printf '%s\n' "$OUT" | KOS_KP_BANNER="$(thread_fault_re forged)" awk '
            on && /^=== / { exit }
            on && /^  PC=0x[0-9a-fA-F]+ mcause=0x[0-9a-fA-F]+$/ { print; exit }
            index($0, ENVIRON["KOS_KP_BANNER"]) > 0 { on = 1 }')"
        pc="$(printf '%s' "$record" | sed -n 's/^  PC=0x\([0-9a-fA-F]*\) .*/\1/p')"
        cause="$(printf '%s' "$record" | sed -n 's/.* mcause=0x\([0-9a-fA-F]*\)$/\1/p')"
        if [ -z "$pc" ] || [ "$((0x$pc))" -ne "$((0x$entry))" ]; then
            cfail kill-pc "mppforge: 'forged' was killed at PC 0x$pc, not at its entry 0x$entry"
        fi
        if [ -z "$cause" ] || [ "$((0x$cause))" -ne 2 ]; then
            cfail kill-cause "mppforge: 'forged' was killed on mcause 0x$cause, not an illegal instruction"
        fi
        assert_no_panic "mppforge: the child was killed AND the system panicked"
        status_clause "the system exited 0 once main returned" 0
        echo "PASS: 'forged' died from U-mode at its entry 0x$entry, line $killed; main ran at line $survived"
        ;;
    div0 | ldiv0)
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            cfail survived "$arm: main never ran again after the worker divided by zero"
        fi
        assert_no_panic "$arm: the divide by zero ended the system"
        quotient="$(line_of "\[fs\] divide by zero returned ")"
        case "$outcome" in
            killed)
                if [ -n "$quotient" ]; then
                    cfail trapped "$arm: the division returned a quotient where $arch traps it"
                fi
                killed="$(line_of "$(thread_fault_re faulter)")"
                if [ -z "$killed" ]; then
                    cfail killed "$arm: no thread-kill for 'faulter'"
                fi
                after_about "$killed" "the kill"
                if [ "$survived" -le "$killed" ]; then
                    cfail order "$arm: main's line is at $survived, not after the kill at $killed"
                fi
                # <status name>: the hex the kill record carries after it, searched from the kill.
                record_status() {
                    printf '%s\n' "$OUT" | sed -n "$killed,\$p" \
                        | sed -n "s/.* $1=0x\([0-9a-fA-F]*\).*/\1/p" | head -n1
                }
                case "$arch" in
                    armv7m)
                        # UFSR.DIVBYZERO, CFSR bit 25, for the hardware divide and the hook's alike.
                        st="$(record_status CFSR)"
                        if [ -z "$st" ] || [ $(( 0x$st & 0x2000000 )) -eq 0 ]; then
                            cfail cause "$arm: the kill record's CFSR=0x$st carries no DIVBYZERO"
                        fi
                        why="CFSR=0x$st"
                        ;;
                    armv6m)
                        st="$(record_status DIVBYZERO)"
                        if [ "$st" != 1 ]; then
                            cfail cause "$arm: the kill record does not name the hook's UDF (DIVBYZERO=0x$st)"
                        fi
                        why="DIVBYZERO=0x1"
                        ;;
                    x86_64)
                        # Vector 0, #DE, in the high half, and no error code.
                        st="$(record_status vec:err)"
                        if [ -z "$st" ] || [ $(( 0x$st )) -ne 0 ]; then
                            cfail cause "$arm: the kill record's vec:err=0x$st is not #DE"
                        fi
                        why="vec:err=0x0"
                        ;;
                    sim)
                        # FPE_INTDIV.
                        st="$(record_status "SIGFPE si_code")"
                        if [ "$st" != 1 ]; then
                            cfail cause "$arm: the kill record is not SIGFPE FPE_INTDIV (si_code=0x$st)"
                        fi
                        why="SIGFPE si_code=0x1"
                        ;;
                    *)
                        fail "$arm: no divide-by-zero record is defined for arch '$arch'"
                        ;;
                esac
                status_clause "the system exited 0 once main returned" 0
                echo "PASS: 'faulter' died dividing by zero ($why) at line $killed and main ran at line $survived"
                ;;
            returned:*)
                want="${outcome#returned:}"
                if has_e "$(thread_fault_re faulter)"; then
                    cfail returned "$arm: the worker was killed, where $arch returns a quotient"
                fi
                if [ -z "$quotient" ]; then
                    cfail returned "$arm: the worker printed no quotient"
                fi
                after_about "$quotient" "the quotient"
                if [ "$survived" -le "$quotient" ]; then
                    cfail order "$arm: main's line is at $survived, not after the quotient at $quotient"
                fi
                got="$(printf '%s\n' "$OUT" | sed -n "${quotient}p" \
                    | sed -n 's/.*divide by zero returned \(-\{0,1\}[0-9][0-9]*\).*/\1/p')"
                if [ "$want" != any ] && [ "$got" != "$want" ]; then
                    cfail quotient "$arm: the quotient is '$got', where $arch defines $want"
                fi
                status_clause "the system exited 0 once main returned" 0
                echo "PASS: 'faulter' divided by zero to $got on $arch and main ran at line $survived"
                ;;
            *)
                fail "unknown outcome '$outcome': expected 'killed' or 'returned:<quotient|any>'"
                ;;
        esac
        ;;
    stkof)
        # The overflow on a core with stack limit registers: the recursion's own push, or the
        # entry that would stack below the limit, raises STKOF before a byte lands under it,
        # so the thread is killed and the system carries on. Keyed on the kill's own record,
        # and a stacking-abort bit beside STKOF means the MPU and not the limit stopped it.
        [ "$outcome" = contained ] || fail "stkof: the only outcome this arm has is 'contained'"
        killed="$(line_of "$(thread_fault_re faulter)")"
        if [ -z "$killed" ]; then
            cfail killed "stkof: no thread-kill for 'faulter', so the stack limit did not stop the overflow"
        fi
        after_about "$killed" "the kill"
        survived="$(line_of "\[fs\] survivor ran after the fault")"
        if [ -z "$survived" ]; then
            cfail survived "stkof: main never ran again after the overflow"
        fi
        if [ "$survived" -le "$killed" ]; then
            cfail order "stkof: main's line is at $survived, not after the kill at $killed"
        fi
        cfsr="$(printf '%s\n' "$OUT" | sed -n "$killed,\$p" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
        [ -n "$cfsr" ] || cfail cause "stkof: the kill record carries no CFSR"
        if [ $(( 0x$cfsr & 0x100000 )) -eq 0 ]; then
            cfail cause "stkof: CFSR=0x$cfsr has no STKOF, so something other than the limit stopped the overflow"
        fi
        if [ $(( 0x$cfsr & 0x1818 )) -ne 0 ]; then
            cfail cause "stkof: CFSR=0x$cfsr carries a stacking abort, so a write below the limit reached the bus"
        fi
        assert_no_panic "stkof: the worker was killed AND the system panicked"
        status_clause "the system exited 0 once main returned" 0
        echo "PASS: 'faulter' overflowed into STKOF (CFSR=0x$cfsr), died at line $killed, main ran at line $survived"
        ;;
    overflow | offstack | kwrite | misalign | unread | fpunread | wildspin | wildsvc | storesvc | bxns | blxns | bxnsrom)
        if [ "$outcome" = killed ]; then
            killed="$(line_of "$(thread_fault_re faulter)")"
            if [ -z "$killed" ]; then
                cfail killed "$arm: no thread-kill for 'faulter' (the fault ended the system?)"
            fi
            after_about "$killed" "the kill"
            survived="$(line_of "\[fs\] survivor ran after the fault")"
            if [ -z "$survived" ]; then
                cfail survived "$arm: main never ran again after the worker faulted"
            fi
            if [ "$survived" -le "$killed" ]; then
                cfail order "$arm: main's line is at $survived, not after the kill at $killed"
            fi
            assert_no_panic "$arm: the worker was killed AND the system panicked"
            if has_e "CONTAINED \\(wild"; then
                cfail svc "$arm: a wild-stack refusal followed the kill, so an exception the fault left
    pending ran on the dead worker's exit"
            fi
            if [ "$arm" = wildsvc ] || [ "$arm" = storesvc ]; then
                if ! kill_record | grep -qx "  SVCall dropped"; then
                    cfail svc "$arm: the kill dropped no pending SVCall, so the fault was taken before
    the svc and this arm witnessed nothing"
                fi
                if has "kword] CORRUPTED"; then
                    cfail kwrite "$arm: main's word changed under the worker"
                fi
                intact="$(line_of "\[fs\] \[kword\] INTACT")"
                if [ -z "$intact" ] || [ "$intact" -le "$survived" ]; then
                    cfail kwrite "$arm: main printed no verdict on its word after its line"
                fi
            fi
            # Read off the kill record, so an unrelated fault killing the worker cannot pass.
            record="$(printf '%s\n' "$OUT" | sed -n "$killed,\$p")"
            case "$arch:$arm" in
                rxv3:overflow)
                    sts="$(printf '%s\n' "$record" | sed -n 's/.*MPESTS=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                    [ -n "$sts" ] || cfail cause "overflow: the kill record carries no MPESTS, so no RX MPU error killed the worker"
                    if [ $(( 0x$sts & 0x6 )) -ne 6 ]; then
                        cfail cause "overflow: MPESTS=0x$sts is not a denied operand write (DMPER and DRW)"
                    fi
                    why="MPESTS=0x$sts, a denied write"
                    ;;
                rxv3:offstack)
                    cause="$(printf '%s\n' "$record" | sed -n 's/.*cause=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                    if [ "$cause" != 50 ]; then
                        cfail cause "offstack: the kill record names cause=0x$cause, not the deliberate privileged instruction (0x50)"
                    fi
                    why="cause=0x50, the privileged instruction"
                    ;;
                armv7m:bxns | armv7m:blxns)
                    sfsr="$(kill_pc_line | sed -n 's/^  PC unknown SFSR=0x\([0-9a-fA-F]*\)$/\1/p')"
                    [ -n "$sfsr" ] || cfail cause "$arm: the kill record is not 'PC unknown' with an SFSR, so the fault was read as a Secure one"
                    if [ $(( 0x$sfsr & 0x1 )) -eq 0 ]; then
                        cfail cause "$arm: SFSR=0x$sfsr carries no INVEP, so Non-secure state never fetched the Secure target"
                    fi
                    why="SFSR=0x$sfsr, a branch into Non-secure state, PC unknown"
                    ;;
                armv7m:bxnsrom)
                    ns="$(kill_pc_line | sed -n 's/^  PC unknown CFSR_NS=0x\([0-9a-fA-F]*\)$/\1/p')"
                    [ -n "$ns" ] || cfail cause "bxnsrom: the kill record is not 'PC unknown' with a CFSR_NS, so no Non-secure fault stopped the branch"
                    if [ $(( 0x$ns & 0x1 )) -eq 0 ]; then
                        cfail cause "bxnsrom: CFSR_NS=0x$ns carries no IACCVIOL, so the Non-secure MPU did not refuse the first fetch"
                    fi
                    why="CFSR_NS=0x$ns, the Non-secure MPU refused the fetch, PC unknown"
                    ;;
                armv7m:storesvc)
                    cfsr="$(printf '%s\n' "$record" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
                    [ -n "$cfsr" ] || cfail cause "storesvc: the kill record carries no CFSR"
                    if [ $(( 0x$cfsr & 0x400 )) -eq 0 ]; then
                        cfail cause "storesvc: CFSR=0x$cfsr carries no IMPRECISERR, so the store faulted precisely and the SVC never met it"
                    fi
                    why="CFSR=0x$cfsr, an imprecise store ahead of the svc"
                    ;;
                armv7m:overflow | armv7m:unread | armv7m:fpunread | armv7m:offstack | armv7m:wildsvc)
                    # No PC: the frame was refused unread, aborted or off the stack.
                    cfsr="$(kill_pc_line | sed -n 's/^  PC unknown CFSR=0x\([0-9a-fA-F]*\)$/\1/p')"
                    [ -n "$cfsr" ] || cfail cause "$arm: the kill record is not 'PC unknown' with a CFSR, so a PC was read from a frame the thread could not be trusted to have stacked"
                    if [ "$arm" = offstack ]; then
                        # The frame WAS written, so the stacking-abort bits must be clear, or
                        # this arm says nothing about the bounds test.
                        if [ $(( 0x$cfsr & 0x1818 )) -ne 0 ]; then
                            cfail cause "offstack: CFSR=0x$cfsr carries a stacking-abort bit, so the bounds test is not what refused this frame"
                        fi
                        why="CFSR=0x$cfsr, frame off the stack, PC unknown"
                    elif [ $(( 0x$cfsr & 0x10 )) -ne 0 ]; then
                        why="CFSR=0x$cfsr, MSTKERR, PC unknown"
                    elif [ "$arm" != overflow ] && [ $(( 0x$cfsr & 0x1000 )) -ne 0 ]; then
                        why="CFSR=0x$cfsr, STKERR, PC unknown"
                    elif [ $(( 0x$cfsr & 0x1000 )) -ne 0 ] \
                        && kill_record | grep -qx "  SYSMPU ISOLATION FAULT"; then
                        # The K64F's SYSMPU, a bus-level unit, aborts the overflow's stacking as
                        # STKERR; its line is what tells that denial from a bare BusFault.
                        why="CFSR=0x$cfsr, STKERR corroborated by SYSMPU ISOLATION FAULT, PC unknown"
                    else
                        cfail cause "$arm: CFSR=0x$cfsr is neither MSTKERR nor a SYSMPU-corroborated STKERR, so the frame was written and this arm witnessed nothing"
                    fi
                    ;;
                armv6m:overflow | armv6m:offstack | armv6m:wildsvc)
                    if printf '%s\n' "$record" | grep -q "CFSR="; then
                        cfail cause "$arm: the kill record carries a CFSR, so it is not an armv6m capture"
                    fi
                    if [ "$(kill_pc_line)" != "  PC unknown" ]; then
                        cfail cause "$arm: the kill record is not a bare 'PC unknown', so a PC was read from a frame off the thread's stack"
                    fi
                    why="PC unknown, no CFSR to latch"
                    ;;
                *)
                    fail "$arm: no killed outcome is defined for arch '$arch'"
                    ;;
            esac
            status_clause "the system exited 0 once main returned" 0
            echo "PASS: $arm killed 'faulter' alone ($why) at line $killed and main ran at line $survived"
            exit 0
        fi
        if has_e "$(thread_fault_re faulter)" || has_e "$(thread_fault_re fs_wild)"; then
            cfail redirected "$arm: the fault was redirected to the exit stub instead of being refused at the trap entry"
        fi
        # kwrite is the trap-stack security regression: the worker aimed its SP at a word of
        # main's data. A backend that stored the frame through the U-mode SP changed that word,
        # and main says so once it outlives the refusal.
        if { [ "$arm" = kwrite ] || [ "$arm" = wildspin ]; } && has "kword] CORRUPTED"; then
            cfail kwrite "$arm: the kernel stored through the U-mode SP into main's word"
        fi
        if [ "$outcome" = contained ]; then
            # A contained refusal spells its banner CONTAINED, which check_panic_banners keeps
            # OUT of tests/lib/panic.ere. Without this a terminal panic AFTER the survivor line
            # satisfied every clause below.
            assert_no_panic "$arm: something panicked on a run that was supposed to contain"
            refused="$(line_of "$(wild_refusal_re "$arch")")"
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
            if [ "$arm" = wildspin ]; then
                gone="$(line_of "\[fs\] \[wildspin\] the spinner is gone")"
                if [ -z "$gone" ]; then
                    cfail gone "wildspin: the faulter never saw its spinner end"
                fi
                if [ "$gone" -le "$refused" ] || [ "$gone" -ge "$survived" ]; then
                    cfail order "wildspin: the spinner's end at $gone is not between the refusal at
    $refused and main's line at $survived"
                fi
            fi
            # A missing verdict is main reaching the readback and printing neither, the silent
            # arm this pair exists to refuse.
            if [ "$arm" = kwrite ] || [ "$arm" = wildspin ]; then
                intact="$(line_of "\[fs\] \[kword\] INTACT")"
                if [ -z "$intact" ]; then
                    cfail kwrite "$arm: main printed no verdict on its word, so nothing here
    read it"
                fi
                if [ "$intact" -le "$survived" ]; then
                    cfail order "$arm: the word's verdict at $intact is not after main's line at
    $survived, so it was read before the fault it is meant to judge"
                fi
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
                    kwrite:*[048cC] | wildspin:*[048cC]) ;;
                    kwrite:* | wildspin:*)
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
        fail "unknown outcome '$outcome': expected 'contained' or 'killed'"
        ;;
    *)
        fail "$_usage"
        ;;
esac
exit 0
