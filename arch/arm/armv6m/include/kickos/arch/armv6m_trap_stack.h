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
     _SVCK   808  picopi-st at 628, the deepest of this arch's four declared presets
                  (picopi 572, microbit and picopi-flat 504).

   THE PANIC REPORTER IS NOT ON THIS CHAIN. kpanic leaves this stack before it prints
   (kickos_panic_stack_enter, switch.S), so what 628 measures at picopi-st is grant admission
   under thread_create_call and no console backend at all:
     syscall_dispatch[112] -> thread_create_call[240] -> thread_create[72] -> task_for[24]
     -> domain_for[32] -> grant_region_admissible[32] -> grant_hits_reserved[96]
     -> arch_reserved_blocks[20]
   808 IS ENFORCED OVER THAT 628 ON PURPOSE, as headroom a future change is measured against
   rather than slack to spend. Cutting it to the measurement returns 180 per
   KICKOS_THREAD_SLOTS, the block falling from 896 to 720, and buys the next added assert a
   gate failure on whichever preset happens to be deepest.

   On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with the
   return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a chain is
   not missing a per-level 4.

   tests/static/trap_redzone_indirect.txt binds every reachable indirect site to the one slot
   that call reaches, and the gate refuses to answer while a reachable site is unbound.

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
   kickos_thread_slay_exit. 384, identical on all four presets: what the fault stub descends is
   the pick its teardown's wake performs, ending in the 64-bit divide arch_timer_arm needs.
     kickos_thread_fault_exit[32] -> exit_current[48] -> cap_teardown[40] -> teardown_entry[40]
     -> obj_close_protocol[32] -> mutex_force_unlock[16] -> wake[16] -> resched_after_wake[16]
     -> pick_and_seat[16] -> ktime_rearm[16] -> arch_timer_arm[32] -> __aeabi_ldivmod[96]

   NO POSTURE LADDER on this arch: it has neither a telemetry nor a bench variant, so one
   figure covers every registered preset. This arch selects
   ARCH_KERNEL_STACKS_MANDATORY, so no kstacks=0 fallback class stands beside it the way one
   does on armv7m.

   IT NEVER BINDS, WHICH IS WHY IT IS ROUNDED LIKE A THREAD-STACK FIGURE. A kernel-block
   figure is normally left at its measurement because it sizes KICKOS_KERNEL_STACK_SIZE and a
   byte there costs KICKOS_THREAD_SLOTS; this class sizes nothing, SVCK winning the block on
   every registered preset, so the reason for that convention does not reach it. 448 stands
   over the 384 measured: 68 + 448 = 516 against 892 usable,
   where SVCK asks 892 exactly, so this would have to grow 376 more before it bound. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_EXITK 448

/* kickos_thread_return ALONE: an ordinary privileged thread's entry returning, with no fault
   and no redirect to relocate it, so it runs at whatever depth the entry returned from on the
   thread's own stack. 504 enforced, 360 measured on picopi: the pick the teardown's wake
   performs, and no longer the panic reporter under it.
     kickos_thread_return[8] -> exit_current[48] -> cap_teardown[40] -> teardown_entry[40]
     -> obj_close_protocol[32] -> mutex_force_unlock[16] -> wake[16] -> resched_after_wake[16]
     -> pick_and_seat[16] -> ktime_rearm[16] -> arch_timer_arm[32] -> __aeabi_ldivmod[96] */
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

/* THE PANIC REPORTER'S OWN STACK, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed, so an assert on EXITK, RET or SVCK costs those figures its call site and
 * nothing under it. THE FAULT REPORTER IS A DIFFERENT CHAIN and is not priced here: it runs in
 * thread context on the dying thread's own block and is what EXITK still measures.
 *
 * FRAME IS THE HARDWARE FRAME and not 0: the entry sets PRIMASK before the move, but PRIMASK
 * does not mask a HardFault, and a wild access inside the reporter stacks that frame on the SP
 * the reporter is on. A plain integer because check_trap_redzone.sh scrapes it as an immediate;
 * arch_armv6m.cc asserts it against KICKOS_ARMV6M_TRAP_FRAME.
 *
 * 152 MEASURED on the three picopi presets and 144 on microbit, under an enforced 320. */
#define KICKOS_ARMV6M_PANIC_FRAME 32
#define KICKOS_ARMV6M_PANIC_DEPTH 320

#endif /* KICKOS_ARCH_ARMV6M_TRAP_STACK_H */
