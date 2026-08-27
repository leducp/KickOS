/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the ARMv6-M trap sites (switch.S) reserve: one at the BOTTOM of a thread's
 * stack before they write through the live PSP, one at the top of that thread's per-thread
 * KERNEL block, which the syscall trap transfers to.
 *
 * AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame with the
 * PRE-exception privilege, so on a chip with an MPU a kernel-aimed PSP faults on entry. The
 * {r4-r11} block pushed BELOW that frame is written in handler mode and refused by nothing,
 * so a PSP 32 bytes above a stack's base clears the hardware check and still writes under the
 * stack, where r10 and r11 land on the neighbouring thread's stacked PC and xPSR. So the
 * bound is on the room REMAINING below the PSP. nrf51 has no MPU, so nothing refuses the
 * hardware frame there either.
 *
 * Four classes, separated by WHICH STACK they land on:
 *   PENDSV  the switcher's push on the interrupted thread's stack. PendSV_Handler runs in
 *           HANDLER mode, where ARMv6-M forces SP_main (ARMv6-M ARM B1.4.1: the mode is the
 *           selector and CONTROL.SPSEL is RAZ/WI in handler mode), so its kernel-descent
 *           term is 0 by handler mode rather than by measurement.
 *   SVC     the syscall trap's writes on the interrupted thread's stack.
 *   SVCK    the syscall dispatch on the caller's kernel block, svc_trampoline having
 *           relocated SP to ctx.kernel_sp before it calls anything.
 *   EXIT    the death path, below.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figures and fails when a worst
 * case exceeds what the class reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance
 * and the gate would still report room to spare.
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
   structural half, handler mode putting the rest on the MSP. */
#define KICKOS_ARMV6M_TRAP_FRAME 32

/* THE SVC CLASS, structural half: the bytes below the guarded PSP written by the hardware
   and one assembly prologue, which no call graph sees. P0 is the PSP the guard validates, P1
   the PSP the SVC's exception return leaves, where svc_trampoline starts.

     P1 = P0 + 32   the hardware frame exception return unstacks. ARMv6-M has no FP
                    extension, so no posture widens it the way an ARMv7-M FP frame widens
                    its 32 to 104.
     -8             svc_trampoline's `push {r0, r1}`, the two scratch registers it needs to
                    reach g_arch_current and load ctx.kernel_sp.
     -32            the hardware frame a device IRQ or SysTick stacks if it preempts that
                    window. Every v6-M device line sits at reset priority 0, above
                    PRIO_SVCALL, and the window runs in THREAD mode, so ARMv6-M stacks it
                    on the PSP.
     -32            the PendSV that tail-chains behind it. Tail-chaining reuses the frame
                    above, so no second hardware frame is charged, and it cannot repeat:
                    PendSV switches away and pops the block on resume, and a further
                    exception nests on the MSP, so at most ONE exception is ever stacked on
                    a thread's PSP.

   THE STKALIGN PAD CANCELS and is not a term: entry spends 4 exactly when the pre-exception
   SP was 4-mod-8, which is also when P1 - 8 is 4-mod-8 and the preempting entry spends its
   own 4. arch_syscall_reg reaches that posture, its two pushes being ten words together.

   The {r4-r11} push of the fastpath arm is NOT added on top: it is an alternative to this
   window, not something below it, and 40 dominates it. arch_armv6m.cc asserts the
   domination. */
#define KICKOS_ARMV6M_TRAP_NEST_SVC 40

/* THE SVCK CLASS, structural half: what stands between the top of a kernel block (T =
   ctx.kernel_sp) and the first byte syscall_dispatch may use, plus what a preemption puts
   below the deepest byte it uses.

     -16   the continuation header svc_trampoline lays at T: a3 (the fifth argument, passed
           on the stack by AAPCS), the caller-return, the PSP the thread resumes on, and the
           word that keeps SP 8-aligned for the call.
     -4    the STKALIGN pad a preempting exception spends. It does NOT cancel here: above it
           is a chain of compiler frames whose sub is a multiple of 4, so the deepest SP can
           be 4-mod-8 on its own.
     -32   the hardware frame a device IRQ or SysTick stacks. The dispatch runs in THREAD
           mode, so ARMv6-M stacks it on the PSP, which is this block.
     -32   the PendSV that tail-chains behind it, by the single-nesting argument above. */
#define KICKOS_ARMV6M_TRAP_NEST_SVCK 84

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.

     _PENDSV   0  handler mode uses SP_main.
     _SVC      0  svc_trampoline moves SP to ctx.kernel_sp before it calls anything, so the
                  whole dispatch tree is measured as _SVCK.
     _SVCK   808  picopi-st, the deeper of this arch's two declared presets (microbit 624).

   THE PANIC TAIL IS COUNTED, and that is what makes 808 rather than 688 the figure. A kernel
   array has no spawn floor to clear, so trap_redzone_roots.txt declares SVCK stack=kernel and
   a stack=kernel class is measured with no exclusion at all. That also makes the figure
   CONSOLE-SHAPED, the tail being the console writer.

   On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with the
   return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a chain is
   not missing a per-level 4.

   The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
   tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
   calls, and the gate refuses to answer while a reachable site is unbound.

   ONE RESIDUAL, picopi's alone: arch_reboot calls the RP2040 bootrom through two pointers
   read out of ROM, which no call graph can follow and the datasheet gives no stack figure
   for, so a reboot syscall descends an unknown depth of ROM code on the calling thread's
   kernel block. trap_redzone_indirect.txt declares those two sites at 0 and states it. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC 0
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVCK 808

/* THE EXIT CLASS, structural half: NEST_SVCK's window without the 16-byte continuation
   header, because no trampoline lays one here. The backend rewrites the exception frame so
   the return lands in the stub with SP at kickos_fault_stack_top, and the stub is entered by
   an exception return rather than by a call.

     -4    the STKALIGN pad, which does not cancel, as in the SVCK window.
     -32   the hardware frame a device IRQ or SysTick stacks. The stub runs in THREAD mode.
     -32   the PendSV that tail-chains behind it.

   A frame term and not 0 because the stub is PREEMPTIBLE: exit_current reschedules, so a
   preemption is the ordinary case here. */
#define KICKOS_ARMV6M_TRAP_NEST_EXIT 68

/* The measured descent of the two stubs a dying thread runs PRIVILEGED on its own KERNEL
   BLOCK, kickos_fault_stack_top answering with ctx.kernel_sp: kickos_thread_fault_exit and
   kickos_thread_slay_exit. 608 on picopi, the fault stub the deeper of the two through
   kprintf_fault. NO POSTURE LADDER on this arch: it has neither a telemetry nor a bench
   variant, so one figure covers every registered preset. This arch selects
   ARCH_KERNEL_STACKS_MANDATORY, so no kstacks=0 fallback class stands beside it the way one
   does on armv7m.

   IT NEVER BINDS: 68 + 608 = 676 against 892 usable, where SVCK asks 892 exactly. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_EXITK 608

/* kickos_thread_return ALONE: an ordinary privileged thread's entry returning, with no fault
   and no redirect to relocate it, so it runs at whatever depth the entry returned from on the
   thread's own stack. 504 on picopi. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_RET 504

/* What each guarded site enforces, in bytes below the live PSP: a class's structural half
   plus its measured one, which is also what the gate compares against KICKOS_MIN_STACK_SIZE.
   Loaded with `ldr rN, =`, out of the literal pool, because a v6-M movs carries an imm8 and a
   figure can outgrow it. */
#define KICKOS_ARMV6M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV6M_TRAP_FRAME + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV)
#define KICKOS_ARMV6M_TRAP_NEED_SVC \
    (KICKOS_ARMV6M_TRAP_NEST_SVC + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC)

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE, not a bound
   anything refuses at run time, every byte of it being written by privileged code through a
   pointer the kernel seated. arch_armv6m.cc static_asserts the block against it, and
   check_trap_redzone.sh compares the same pair against the block the board configured. */
#define KICKOS_ARMV6M_TRAP_NEED_SVCK \
    (KICKOS_ARMV6M_TRAP_NEST_SVCK + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVCK)

/* struct arch_context field offsets the trap sites read as plain displacements. switch.S
   .equ's from these and arch_armv6m.cc static_asserts offsetof against them. The
   telemetry-only trace_tid is the LAST field precisely so that no build posture shifts them,
   which would make the guard compare a PSP against a trace id; kernel_sp sits ahead of it for
   that reason. */
#define KICKOS_ARMV6M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV6M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV6M_CTX_OFF_KERNEL_SP 20
#define KICKOS_ARMV6M_CTX_OFF_TRACE_TID 24

#endif /* KICKOS_ARCH_ARMV6M_TRAP_STACK_H */
