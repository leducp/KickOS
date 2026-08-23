/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the ARMv6-M trap sites (switch.S) reserve, one at the BOTTOM of a thread's
 * stack before they agree to write through the live PSP, one at the top of that thread's
 * per-thread KERNEL block, which the syscall trap transfers to.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame ABOVE
 * the PSP with the PRE-exception privilege, so on a chip with an MPU a kernel-aimed PSP
 * faults on entry before any handler runs. The {r4-r11} block PendSV and the SVC fastpath
 * push BELOW that frame is written in handler mode and is refused by nothing, so a PSP 32
 * bytes above a stack's base clears the hardware check and still writes under the stack,
 * where r10 and r11 land on the neighbouring thread's stacked PC and xPSR. So the bound is
 * on the room REMAINING below the PSP.
 *
 * nrf51 has no MPU, so nothing refuses the hardware frame there either. The guard cannot
 * undo a store that has already landed; what it bounds is every write the handler and the
 * window below it make.
 *
 * THREE CLASSES, and WHICH STACK separates them:
 *
 *   PENDSV  the switcher's push, on the interrupted thread's stack. PendSV_Handler runs in
 *           HANDLER mode, where ARMv6-M forces SP_main (ARMv6-M ARM B1.4.1: the mode is the
 *           selector and CONTROL.SPSEL is RAZ/WI in handler mode), so every bl in it
 *           (kickos_arch_mpu_commit, the telemetry hook) descends on the MSP. Its
 *           kernel-descent term is therefore 0 by handler mode rather than by measurement.
 *
 *   SVC     the syscall trap's own writes on the interrupted thread's stack: the fastpath
 *           arm's callee block, which IS the leaf's argument array, and the eight scratch
 *           bytes svc_trampoline spends before it moves SP onto the kernel block. The
 *           fastpath arm is handler-mode too, so its kickos_ipc_fastpath call descends on
 *           the MSP.
 *
 *   SVCK    the syscall dispatch, on the caller's own kernel block. svc_trampoline arrives
 *           privileged in THREAD mode, relocates SP to ctx.kernel_sp and calls
 *           syscall_dispatch there, so the descent is a requirement on
 *           KICKOS_KERNEL_STACK_SIZE and never on a pointer a thread chose.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figures below and fails when
 * a worst case exceeds what the class reserves. The measured half and the structural half
 * stay SEPARATE macros: folded into one, a growing dispatch could eat the structural
 * allowance and the gate would still report room to spare.
 *
 * CCR.STKALIGN is Read-As-One on ARMv6-M (ARMv6-M ARM B3.2.8, D3.6.1), so exception entry
 * clears bit 2 of the banked SP before stacking and the guarded PSP is 8-byte aligned
 * whatever the thread wrote to SP: a word-alignment test on it is always true and the guard
 * carries no alignment leg.
 */

#ifndef KICKOS_ARCH_ARMV6M_TRAP_STACK_H
#define KICKOS_ARCH_ARMV6M_TRAP_STACK_H

/* The software block PendSV and the SVC fastpath push below the live PSP: {r4-r11}, eight
   words. A REGISTER COUNT, not a measured depth, and arch_armv6m.cc asserts it against the
   register list it prices, which gas cannot count. It is also the PENDSV class's whole
   structural half, handler mode putting the rest on the MSP, so that class gets no second
   name for this figure. */
#define KICKOS_ARMV6M_TRAP_FRAME 32

/* THE SVC CLASS, structural half: the bytes below the guarded PSP whose writers are the
   hardware and one assembly prologue, so no call graph sees them. Write P0 for the PSP the
   guard validates and P1 for the PSP the SVC's own exception return leaves, which is where
   svc_trampoline starts.

     P1 = P0 + 32   the hardware frame that exception return unstacks. ARMv6-M has no FP
                    extension, so no posture widens it the way an ARMv7-M FP frame widens
                    its 32 to 104.
     -8             svc_trampoline's `push {r0, r1}`, the two scratch registers it needs to
                    reach g_arch_current and load ctx.kernel_sp. Those eight bytes sit
                    inside the frame the return just popped, so they cost nothing below P0
                    by themselves; they are here because the window below is measured from
                    them.
     -32            the hardware frame a device IRQ or SysTick stacks if it preempts that
                    window. Every v6-M device line sits at reset priority 0, above
                    PRIO_SVCALL, and the window runs in THREAD mode, so ARMv6-M stacks it
                    on the PSP and nothing checks it: this term is what makes the class a
                    privilege question and not only a sizing one.
     -32            the PendSV that tail-chains behind it and pushes the {r4-r11} block one
                    level lower. Tail-chaining reuses the frame above, so no second
                    hardware frame is charged, and it cannot repeat: PendSV switches away
                    and pops the block on resume, and a further exception nests on the MSP,
                    so at most ONE exception is ever stacked on a thread's PSP.

   32 - 8 - 32 - 32 is -40, so 40 below P0. The STKALIGN pad CANCELS and is not a term:
   entry spends 4 exactly when the pre-exception SP was 4-mod-8, which is also when P1 - 8
   is 4-mod-8 and the preempting entry spends its own 4. arch_syscall_reg reaches that
   posture, its two pushes being ten words together.

   The {r4-r11} push of the fastpath arm is NOT added on top: it is an alternative to this
   window, not something below it, and 40 dominates it. arch_armv6m.cc asserts the
   domination. */
#define KICKOS_ARMV6M_TRAP_NEST_SVC 40

/* THE SVCK CLASS, structural half: what stands between the top of a kernel block and the
   first byte syscall_dispatch may use, plus what a preemption puts below the deepest byte
   it uses. Write T for ctx.kernel_sp, the block's top.

     -16   the continuation header svc_trampoline lays at T: a3 (the fifth argument, which
           the AAPCS passes on the stack), the caller-return it exits to, the PSP the
           thread resumes on, and the word that keeps SP 8-byte aligned for the call.
           syscall_dispatch runs below it.
     -4    the STKALIGN pad a preempting exception spends. It does NOT cancel here the way
           it does in the SVC window: what sits above is a chain of compiler frames, whose
           sub is a multiple of 4, so the deepest SP can be 4-mod-8 on its own.
     -32   the hardware frame a device IRQ or SysTick stacks when it preempts the dispatch.
           The dispatch runs in THREAD mode, so ARMv6-M stacks it on the PSP, which is this
           block.
     -32   the PendSV that tail-chains behind it, by the same single-nesting argument as
           the SVC class above.

   16 + 4 + 32 + 32 is 84. */
#define KICKOS_ARMV6M_TRAP_NEST_SVCK 84

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.

     _PENDSV   0  handler mode uses SP_main, so everything the switcher calls descends
                  there instead of on this stack.
     _SVC      0  svc_trampoline moves SP to ctx.kernel_sp before it calls anything, so
                  syscall_dispatch and the whole tree under it descend on the kernel block
                  and are measured as _SVCK.
     _SVCK   808  measured 808 on picopi-st.

   THE MEASUREMENT is the deepest over the presets tests/static/trap_redzone_roots.txt
   declares for this arch (MinSizeRel, -fcallgraph-info=su,da), taken as the longest
   weighted path over the merged call graph from the one root svc_trampoline branches to.

   THE PANIC TAIL IS COUNTED, and that is what makes 808 rather than 688 the figure. A
   kernel array has no spawn floor to clear, so trap_redzone_roots.txt declares SVCK
   stack=kernel and a stack=kernel class is measured with no exclusion at all. That also
   makes the figure CONSOLE-SHAPED, the tail being the console writer, so it is the worse of
   this arch's registered presets and not one board's reading: microbit 624, picopi-st 808.
   BOTH declared presets are registered on this arch, so the sample is the whole of it here,
   which is not true of armv7m.

   On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with
   the return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a
   chain is not missing a per-level 4.

   The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
   tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source
   line calls, and the gate refuses to answer at all while a reachable site is unbound.

   ONE RESIDUAL, and it is picopi's alone: arch_reboot calls the RP2040 bootrom through two
   pointers read out of ROM, which no call graph can follow and the datasheet gives no stack
   figure for, so a reboot syscall descends an unknown depth of ROM code on the calling
   thread's kernel block. trap_redzone_indirect.txt declares those two sites at 0 and states
   it. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC 0
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVCK 808

/* What each guarded site enforces: room below the live PSP, in bytes. The whole red zone
   for a class, the structural half plus the measured one, which is also what the gate
   compares against KICKOS_MIN_STACK_SIZE. Loaded with `ldr rN, =`, out of the literal pool,
   because a v6-M movs carries an imm8 and a figure can outgrow it. */
#define KICKOS_ARMV6M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV6M_TRAP_FRAME + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV)
#define KICKOS_ARMV6M_TRAP_NEED_SVC \
    (KICKOS_ARMV6M_TRAP_NEST_SVC + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC)

/* What one kernel block has to hold, which is a requirement on KICKOS_KERNEL_STACK_SIZE and
   not a bound anything refuses at run time: every byte of it is written by privileged code
   through a pointer the kernel seated. arch_armv6m.cc static_asserts the block against it,
   and check_trap_redzone.sh compares the same pair against the block the board configured. */
#define KICKOS_ARMV6M_TRAP_NEED_SVCK \
    (KICKOS_ARMV6M_TRAP_NEST_SVCK + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVCK)

/* struct arch_context field offsets the trap sites read as plain displacements. switch.S
   .equ's from these and arch_armv6m.cc static_asserts offsetof against them. UNCONDITIONAL,
   because the telemetry-only trace_tid is the LAST field precisely so that no build posture
   shifts them, which would make the guard compare a PSP against a trace id. kernel_sp sits
   ahead of it for that reason and not after. */
#define KICKOS_ARMV6M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV6M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV6M_CTX_OFF_KERNEL_SP 20
#define KICKOS_ARMV6M_CTX_OFF_TRACE_TID 24

#endif /* KICKOS_ARCH_ARMV6M_TRAP_STACK_H */
