/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent the ARMv6-M trap sites (switch.S) reserve at the BOTTOM of a thread's stack
 * before they agree to write through the live PSP.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. Exception entry stacks the HARDWARE frame ABOVE
 * the PSP with the PRE-exception privilege, so on a chip with an MPU a kernel-aimed PSP
 * faults on entry before any handler runs. The {r4-r11} block PendSV and the SVC trap push
 * BELOW that frame is written in handler mode and is refused by nothing, so a PSP 32 bytes
 * above a stack's base clears the hardware check and still writes under the stack, where
 * r10 and r11 land on the neighbouring thread's stacked PC and xPSR. So the bound is on the
 * room REMAINING below the PSP, and it covers the push PLUS whatever the kernel then runs
 * on that same stack.
 *
 * nrf51 has no MPU, so nothing refuses the hardware frame there either. The guard cannot
 * undo a store that has already landed; what it bounds is every write the handler and the
 * dispatch below it make.
 *
 * TWO CLASSES, because the two guarded sites differ in exactly that second half:
 *
 *   PENDSV  the switcher's push, and nothing else. PendSV_Handler runs in HANDLER mode,
 *           where ARMv6-M forces SP_main (ARMv6-M ARM B1.4.1: the mode is the selector and
 *           CONTROL.SPSEL is RAZ/WI in handler mode), so every bl in it
 *           (kickos_arch_mpu_commit, the telemetry hook) descends on the MSP. Its
 *           kernel-descent term is therefore 0 by handler mode rather than by measurement.
 *           The SVC fastpath arm is handler-mode too, so its kickos_ipc_fastpath call makes
 *           the MSP a kernel-stack SIZING question and not a privilege one.
 *
 *   SVC     the slow path, the one place kernel C runs on a thread-chosen stack.
 *           SVC_Handler rewrites the stacked PC and exception-returns into svc_trampoline,
 *           which runs PRIVILEGED IN THREAD MODE on that same PSP and calls
 *           syscall_dispatch. A privileged access is not checked against the MPU, so the
 *           whole descent lands wherever the thread aimed the PSP.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figure below and fails when
 * the worst case exceeds what it reserves. The measured half and the structural half stay
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

/* The software block both guarded sites push below the live PSP: {r4-r11}, eight words.
   A REGISTER COUNT, not a measured depth, and arch_armv6m.cc asserts it against the
   register list it prices, which gas cannot count. It is also the PENDSV class's whole
   structural half, handler mode putting the rest on the MSP, so that class gets no second
   name for this figure. */
#define KICKOS_ARMV6M_TRAP_FRAME 32

/* THE SVC CLASS, structural half: the bytes below the guarded PSP whose writers are the
   hardware and one assembly prologue, so no call graph sees them. Write P0 for the PSP the
   guard validates.

     -32   the hardware frame the SVC's own exception return UNSTACKS before svc_trampoline
           runs, so the trampoline starts 32 bytes ABOVE P0. ARMv6-M has no FP extension,
           so no posture widens this frame the way an ARMv7-M FP frame widens its 32 to 104.
      +8   svc_trampoline's own `push {r4, lr}`.
      +8   its `sub sp, #8`, the 5th-argument slot. A v6-M push cannot reach r12, so a3
           arrives through r4 and needs a slot of its own, where the ARMv7-M trampoline
           carries a3 inside its `push {r12, lr}`. syscall_dispatch runs below this.
     +36   the hardware frame a device IRQ or SysTick stacks when it preempts the descent.
           That exception is taken FROM THREAD MODE, so ARMv6-M stacks it on the PSP, at the
           bottom of the descent, and nothing checks it: this term is what makes the class a
           privilege question and not only a sizing one. 36 and not 32 because the descent
           can be interrupted at a 4-mod-8 SP, which makes entry pay the STKALIGN
           adjustment.
     +32   the PendSV that tail-chains behind it and pushes the {r4-r11} block one level
           lower. Tail-chaining reuses the frame above, so no second hardware frame is
           charged, and it cannot repeat: PendSV switches away and pops the block on resume,
           and a further exception nests on the MSP, so at most ONE exception is ever
           stacked on a thread's PSP. Reserving it is also what keeps a legitimate deep
           dispatch from being killed by the PENDSV guard, which charges its own 32 below
           that same PSP.

   The {r4-r11} push of the fastpath arm is NOT added on top: it is an alternative to this
   descent, not something below it, and 52 dominates it. arch_armv6m.cc states the
   domination. */
#define KICKOS_ARMV6M_TRAP_NEST_SVC 52

/* Worst-case bytes the kernel's C dispatch descends BELOW all of that, per class.

     _PENDSV    0  handler mode uses SP_main, so everything the switcher calls descends
                   there instead of on this stack.
     _SVC     688  measured 676, rounded up to a 16-byte boundary.

   THE MEASUREMENT is the deepest over the presets tests/static/trap_redzone_roots.txt
   declares for this arch (MinSizeRel, -fcallgraph-info=su,da), taken as the longest
   weighted path over the merged call graph from the one root svc_trampoline branches to.
   676 on picopi-st:

     syscall_dispatch[32] -> syscall_body[120] -> thread_spawn[248] -> thread_create[72]
     -> task_for[24] -> domain_for[32] -> grant_region_admissible[32]
     -> grant_hits_reserved[96] -> arch_reserved_blocks[20]

   microbit measures 460 on a shorter chain: it has no MPU, so domain_for reaches a memset
   rather than the reserved-block walk, and syscall_body is inlined into syscall_dispatch.

   On arm-none-eabi the compiler's reported frame is the prologue push plus the sub, with
   the return address arriving in LR and no incoming slot, so a frameless leaf is 0 and a
   chain is not missing a per-level 4.

   The winning chain runs through an INDIRECT call, the SchedPolicy hook table:
   tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source
   line calls, and the gate refuses to answer at all while a reachable site is unbound.

   EXCLUDED, and the residual it leaves: the noreturn kpanic tail, which every
   KICKOS_ASSERT in the dispatch reaches. Including it takes the deepest measurement to
   808 (picopi-st), which would put the red zone at 860, above the floor, so a thread
   spawned there could not make a syscall at all. The residual is that a kernel assertion
   firing while a thread is parked at the very bottom of its red zone has the console
   writer descend below stack_lo, privileged, with the system already terminating.
   check_trap_redzone.sh prints both figures.

   ONE MORE RESIDUAL, and it is picopi's alone: arch_reboot calls the RP2040 bootrom
   through two pointers read out of ROM, which no call graph can follow and the datasheet
   gives no stack figure for, so a reboot syscall descends an unknown depth of ROM code on
   the calling thread's stack. trap_redzone_indirect.txt declares those two sites at 0 and
   states it. */
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV 0
#define KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC 688

/* What each guarded site enforces: room below the live PSP, in bytes. The whole red zone
   for a class, the structural half plus the measured one, which is also what the gate
   compares against KICKOS_MIN_STACK_SIZE, no run-time term being added on this arch.
   Loaded with `ldr rN, =`, out of the literal pool, because the SVC figure is well past the
   imm8 a v6-M movs can carry. */
#define KICKOS_ARMV6M_TRAP_NEED_PENDSV \
    (KICKOS_ARMV6M_TRAP_FRAME + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV)
#define KICKOS_ARMV6M_TRAP_NEED_SVC \
    (KICKOS_ARMV6M_TRAP_NEST_SVC + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC)

/* struct arch_context field offsets the guard reads as plain displacements. switch.S .equ's
   from these and arch_armv6m.cc static_asserts offsetof against them. UNCONDITIONAL,
   because the telemetry-only trace_tid is the LAST field precisely so that no build posture
   shifts them, which would make the guard compare a PSP against a trace id. kernel_sp sits
   ahead of it for that reason and not after. */
#define KICKOS_ARMV6M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV6M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV6M_CTX_OFF_KERNEL_SP 20
#define KICKOS_ARMV6M_CTX_OFF_TRACE_TID 24

#endif /* KICKOS_ARCH_ARMV6M_TRAP_STACK_H */
