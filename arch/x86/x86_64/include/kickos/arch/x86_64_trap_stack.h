/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the x86_64 entries (trap_x86_64.S, switch.S) require of the stacks they build frames on.
 * Plain integer #defines only: the gate reads each one as an immediate.
 *
 * A RING 3 ENTRY BUILDS ITS FRAME ON THE THREAD'S KERNEL BLOCK: the interrupt gate through the
 * task-state segment's rsp0, the syscall entry through the per-core block. The gate clears the
 * interrupt flag and IA32_FMASK clears it for SYSCALL, so nothing nests on a block under IRQK or
 * SYSK. A RING 0 INTERRUPT RUNS ITS WHOLE DISPATCH ON THE STACK IT INTERRUPTED: idle's, a
 * privileged thread's own, or a death-path stub's block. The double fault, the NMI and the
 * machine check take interrupt-stack-table slots of their own (desc.h).
 *
 * A PRIVILEGED CALLER'S SYSCALL IS A CALL: karch_syscall jumps straight to the dispatch on the
 * caller's own stack with interrupts as they were.
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock and the
 * frame it saves carries that masked flag. So each descent with interrupts live is two classes,
 * the interrupt nested at its deepest byte with the switch cut, and the switch with nothing
 * below it.
 *
 * Each figure is the deepest reading over the x86_64 presets at its core count AND over the
 * compilers the tree builds with: CI's g++ 13 frames run deeper than g++ 16's on the syscall
 * and interrupt chains, and a reservation holds for both. Thread-stack figures are rounded
 * up to the next multiple of 64, kernel-block ones stay at the measurement, and the arrays
 * sized once per image take the next multiple of 64 strictly above. x86_64 gcc counts a
 * function's own return-address slot in its frame, so the assembly bodies below do too.
 */

#ifndef KICKOS_ARCH_X86_64_TRAP_STACK_H
#define KICKOS_ARCH_X86_64_TRAP_STACK_H

/* struct trap_frame, the frame every entry builds from a 16-byte-aligned top: five hardware
 * words, the stub's error code and vector, fifteen registers. arch_x86_64.cc asserts it. */
#define KICKOS_X86_64_TRAP_FRAME 176

/* The same frame delivered at ring 0 onto the stack it interrupts, which the processor first
 * aligns down to 16 bytes (Intel SDM Vol 3, 6.14.2): up to 8 more. */
#define KICKOS_X86_64_TRAP_FRAME_IRQ 184

/* The frame term where nothing interrupts: a class ending in the masked switch. */
#define KICKOS_X86_64_TRAP_FRAME_NONE 0

/* IRQ, IRQK and the frame term of every class an interrupt lands under. 408 on
 * qemu-x86_64-bench under g++ 13 and 344 under g++ 16, a timer expiry's wake re-arming the
 * slice through pick_and_seat, whose switch loads the incoming port set. */
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 448

/* A ring 0 interrupt's whole extent below the rsp it interrupts, FRAME_IRQ + DEPTH_IRQ, which
 * arch_x86_64.cc asserts. */
#define KICKOS_X86_64_TRAP_NEST 632

/* The ring 3 syscall on the block. 1800 on qemu-x86_64-bench under g++ 13, a spawn seeding the
 * new task's space:
 *   syscall_dispatch[112] -> thread_create_call[32] -> spawn_masked[464] -> thread_create[128]
 *   -> task_for[32] -> domain_for[64] -> claim_slot[32] -> aspace_image_seed[112]
 *   -> arch_aspace_map[96] -> map_into[112] x5 -> kickos_frame_alloc[16] -> ... */
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 1800

/* The same dispatch on a privileged caller's own stack with an interrupt nested below: 1800 on
 * qemu-x86_64-bench under g++ 13, down SYSK's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIV 1856

/* The same dispatch through the switch: 1800 on qemu-x86_64-bench under g++ 13, down SYSPRIV's
 * chain. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW 1856

/* The double-fault, NMI and machine-check slots: kickos_x86_64_trap on a static array, 408
 * down IRQ's chain, which bounds the reporter those vectors actually take. */
#define KICKOS_X86_64_TRAP_DEPTH_IST 448

/* The fault and slay stubs on the block with an interrupt nested below: 816 on both presets, a
 * dying task's teardown releasing its address space down the map editor's walks. */
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 816

/* The same stubs through the switch: 816 on both presets, down EXITK's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 816

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below: 816
 * on both presets, down EXITK's chain from kickos_thread_return[16]. */
#define KICKOS_X86_64_TRAP_DEPTH_RET 832

/* The same return through the switch: 816 on both presets, down EXITKSW's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 832

/* idle_entry's own frame and arch_idle_wait's, above the interrupt idle waits for: 24 on both
 * presets. */
#define KICKOS_X86_64_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array, below the null return word kickos_panic_stack_enter
 * pushes. 320 on qemu-x86_64-bench under g++ 13:
 *   kickos_panic_report[16] -> kputs[16] -> kconsole_write[8] -> console_emit[64] -> ...
 *   -> com1_putc[8] -> com1_slot_free[8] */
#define KICKOS_X86_64_PANIC_FRAME 8
#define KICKOS_X86_64_PANIC_DEPTH 384

/* The SMP doorbell, lock wait and route service lengthen reachable call chains. These bounds
 * cover qemu-x86_64-smp12's callgraph after every reachable indirect site was bound, the
 * deepest of the four SMP presets: SYSK's spawn 2296 under g++ 13, IRQ
 * and IST 592 and PANIC 536 under g++ 13, EXITK's teardown 1080 and RET 1064 under both. */
#if KICKOS_KERNEL_CORES > 1
#undef KICKOS_X86_64_TRAP_DEPTH_IRQ
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 640
#undef KICKOS_X86_64_TRAP_NEST
#define KICKOS_X86_64_TRAP_NEST 824
#undef KICKOS_X86_64_TRAP_DEPTH_SYSK
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 2296
/* SYSK, SYSPRIV and SYSPRIVSW: 2296 on qemu-x86_64-smp12 under g++ 13, spawn_masked[480]. */
#undef KICKOS_X86_64_TRAP_DEPTH_SYSPRIV
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIV 2304
#undef KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW
#define KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW 2304
#undef KICKOS_X86_64_TRAP_DEPTH_IST
#define KICKOS_X86_64_TRAP_DEPTH_IST 640
#undef KICKOS_X86_64_TRAP_DEPTH_EXITK
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 1080
#undef KICKOS_X86_64_TRAP_DEPTH_EXITKSW
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 1080
#undef KICKOS_X86_64_TRAP_DEPTH_RET
#define KICKOS_X86_64_TRAP_DEPTH_RET 1088
#undef KICKOS_X86_64_TRAP_DEPTH_RETSW
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 1088
#undef KICKOS_X86_64_PANIC_DEPTH
#define KICKOS_X86_64_PANIC_DEPTH 576
#endif

#endif
