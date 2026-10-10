/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the x86_64 entries (trap_x86_64.S, switch.S) require of the stacks they build frames on.
 * Plain integer #defines only: the gate reads each one as an immediate.
 *
 * A RING 3 ENTRY BUILDS ITS FRAME ON THE THREAD'S KERNEL BLOCK: the interrupt gate through the
 * task-state segment's rsp0, the syscall entry through the per-core block. The gate clears the
 * interrupt flag and IA32_FMASK clears it for SYSCALL, so nothing nests on a block under IRQK,
 * and under SYSK only below arch_irq_window (SYSWIN). A RING 0 INTERRUPT RUNS ITS WHOLE
 * DISPATCH ON THE STACK IT INTERRUPTED: idle's, a privileged thread's own, or a death-path stub's
 * block. The double fault, the NMI and the
 * machine check take interrupt-stack-table slots of their own (desc.h).
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock and the
 * frame it saves carries that masked flag. So each descent with interrupts live is two classes,
 * the interrupt nested at its deepest byte with the switch cut, and the switch with nothing
 * below it.
 *
 * Each figure is the deepest reading over the x86_64 presets at its core count AND over the
 * compilers the tree builds with: CI's g++ 13 frames run deeper than g++ 16's on the syscall
 * and interrupt chains, and a reservation holds for both. Each class is reserved above that
 * reading, at the next multiple of 64 or at a round figure, and the arrays sized once per image
 * take the next multiple of 64 strictly above. x86_64 gcc counts a
 * function's own return-address slot in its frame, so the assembly bodies below do too.
 */

#ifndef KICKOS_ARCH_X86_64_TRAP_STACK_H
#define KICKOS_ARCH_X86_64_TRAP_STACK_H

/* The KickOS toolchain's x86_64-elf GCC 16.2 (docs/design-m10-toolchain.md) builds deeper
 * frames than the host compilers the figures below were first measured under. Where it did, the
 * class is reserved at a round figure above its measurement. On qemu-x86_64-bench it measures
 * IRQ, IRQK and IST 576, EXITK and EXITKSW 920, RET and RETSW 904; on qemu-x86_64-smp12 IRQ 672,
 * EXITK 1184, RET 1152, SYSK 2160 and PANIC 576. */

/* struct trap_frame, the frame every entry builds from a 16-byte-aligned top: five hardware
 * words, the stub's error code and vector, fifteen registers. arch_x86_64.cc asserts it. */
#define KICKOS_X86_64_TRAP_FRAME 176

/* The same frame delivered at ring 0 onto the stack it interrupts, which the processor first
 * aligns down to 16 bytes (Intel SDM Vol 3, 6.14.2): up to 8 more. */
#define KICKOS_X86_64_TRAP_FRAME_IRQ 184

/* The frame term where nothing interrupts: a class ending in the masked switch. */
#define KICKOS_X86_64_TRAP_FRAME_NONE 0

/* A ring 3 entry writes nothing on the thread's own stack: the syscall entry leaves it with no
 * push or dereference, the interrupt gate delivers onto rsp0 or an IST slot, and -mno-red-zone
 * leaves no live byte below rsp. */
#define KICKOS_X86_64_TRAP_ENTRY_FRAME 0
#define KICKOS_X86_64_TRAP_NEED_USER 0

/* IRQ, IRQK and the frame term of every class an interrupt lands under. 408 on
 * qemu-x86_64-bench under g++ 13 and 344 under g++ 16, a timer expiry's wake re-arming the
 * slice through pick_and_seat, whose switch loads the incoming port set. 544 on qemu-x86_64
 * once the image links newlib (docs/design-m10-toolchain.md section 5.5): the same wake's
 * pick_and_seat primes the incoming thread's libc state, reent_prime copying into its space
 * through arch_aspace_acquire. 592 on qemu-x86_64-bench, whose bench brackets widen
 * pick_and_seat's frame on that chain. */
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 640

/* A ring 0 interrupt's whole extent below the rsp it interrupts, FRAME_IRQ + DEPTH_IRQ, which
 * arch_x86_64.cc asserts. */
#define KICKOS_X86_64_TRAP_NEST 824

/* The ring 3 syscall on the block, reserved at 2048. 1880 on qemu-x86_64, a spawn staging 9
 * grants and seeding the new task's space, and 1896 on qemu-x86_64-bench, whose 32-slot root
 * table widens spawn_masked to 528; arch_x86_64.cc refuses more than 9 grants:
 *   syscall_dispatch[144] -> thread_create_call[32] -> spawn_masked[512] -> thread_create[128]
 *   -> task_for[32] -> domain_for[80] -> claim_slot[48] -> aspace_image_seed[144]
 *   -> arch_aspace_map[112] -> map_into[112] x5 -> kickos_frame_alloc[32] -> ... */
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 2048

/* An interrupt nested below arch_irq_window, the one place a ring 3 syscall opens interrupts on
 * the block: the syscall entry's frame and a ring 0 interrupt's whole extent, FRAME + NEST, which
 * arch_x86_64.cc asserts. */
#define KICKOS_X86_64_TRAP_WINDOW 1000

/* The syscall's descent to arch_irq_window, above SYSWIN's frame. 376 on qemu-x86_64-bench and
 * the multicore presets, 360 on qemu-x86_64:
 *   syscall_dispatch[144] -> presync_prepare[16] -> presync_run[176]
 *   -> GranuleWindows::granule[32] -> arch_irq_window[8]
 * WINDOW + 512 = 1512 against 4092 above the canary, and 1128 + 512 = 1640 on the multicore
 * presets. */
#define KICKOS_X86_64_TRAP_DEPTH_SYSWIN 512

/* The double-fault, NMI and machine-check slots: kickos_x86_64_trap on a static array, 408
 * down IRQ's chain, which bounds the reporter those vectors actually take, and 544 with newlib
 * linked, down the same chain, and 592 on qemu-x86_64-bench. */
#define KICKOS_X86_64_TRAP_DEPTH_IST 640

/* The fault and slay stubs on the block with an interrupt nested below: 816 on both presets, a
 * dying task's teardown releasing its address space down the map editor's walks. */
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 1024

/* The same stubs through the switch: 816 on both presets, down EXITK's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 1024

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below: 816
 * on both presets, down EXITK's chain from kickos_thread_return[16]. */
#define KICKOS_X86_64_TRAP_DEPTH_RET 1024

/* The same return through the switch: 816 on both presets, down EXITKSW's chain. */
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 1024

/* idle_entry's own frame and arch_idle_wait's, above the interrupt idle waits for: 24 on both
 * presets. */
#define KICKOS_X86_64_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array, below the null return word kickos_panic_stack_enter
 * pushes. 312 on qemu-x86_64 and qemu-x86_64-bench:
 *   kickos_panic_report[16] -> kputs[16] -> kconsole_write[8] -> console_emit[64] -> ...
 *   -> com1_putc[8] -> com1_slot_free[8] */
#define KICKOS_X86_64_PANIC_FRAME 8
#define KICKOS_X86_64_PANIC_DEPTH 384

/* The SMP doorbell, lock wait and route service lengthen reachable call chains. These bounds
 * cover qemu-x86_64-smp12's callgraph after every reachable indirect site was bound, the
 * deepest of the four SMP presets: SYSK's spawn 2336, IRQ
 * and IST 592 and PANIC 536 under g++ 13, EXITK's teardown 1080 and RET 1064 under both. */
#if KICKOS_KERNEL_CORES > 1
#undef KICKOS_X86_64_TRAP_DEPTH_IRQ
#define KICKOS_X86_64_TRAP_DEPTH_IRQ 768
#undef KICKOS_X86_64_TRAP_NEST
#define KICKOS_X86_64_TRAP_NEST 952
#undef KICKOS_X86_64_TRAP_WINDOW
#define KICKOS_X86_64_TRAP_WINDOW 1128
#undef KICKOS_X86_64_TRAP_DEPTH_SYSK
#define KICKOS_X86_64_TRAP_DEPTH_SYSK 2432
/* SYSK measures 2160 on qemu-x86_64-smp12, a spawn seeding the new task's space from a 32-slot
 * root table. */
#undef KICKOS_X86_64_TRAP_DEPTH_IST
#define KICKOS_X86_64_TRAP_DEPTH_IST 768
#undef KICKOS_X86_64_TRAP_DEPTH_EXITK
#define KICKOS_X86_64_TRAP_DEPTH_EXITK 1280
#undef KICKOS_X86_64_TRAP_DEPTH_EXITKSW
#define KICKOS_X86_64_TRAP_DEPTH_EXITKSW 1280
#undef KICKOS_X86_64_TRAP_DEPTH_RET
#define KICKOS_X86_64_TRAP_DEPTH_RET 1280
#undef KICKOS_X86_64_TRAP_DEPTH_RETSW
#define KICKOS_X86_64_TRAP_DEPTH_RETSW 1280
#undef KICKOS_X86_64_PANIC_DEPTH
#define KICKOS_X86_64_PANIC_DEPTH 640
#endif

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class IRQ       frame=KICKOS_X86_64_TRAP_FRAME_IRQ depth=KICKOS_X86_64_TRAP_DEPTH_IRQ
 * class IRQK      frame=KICKOS_X86_64_TRAP_FRAME depth=KICKOS_X86_64_TRAP_DEPTH_IRQ stack=kernel
 * class SYSK      frame=KICKOS_X86_64_TRAP_FRAME depth=KICKOS_X86_64_TRAP_DEPTH_SYSK stack=kernel
 * class SYSWIN    frame=KICKOS_X86_64_TRAP_WINDOW depth=KICKOS_X86_64_TRAP_DEPTH_SYSWIN stack=kernel at=arch_irq_window
 * class IST       frame=KICKOS_X86_64_TRAP_FRAME depth=KICKOS_X86_64_TRAP_DEPTH_IST stack=trap
 * class EXITK     frame=KICKOS_X86_64_TRAP_NEST depth=KICKOS_X86_64_TRAP_DEPTH_EXITK stack=kernel
 * class EXITKSW   frame=KICKOS_X86_64_TRAP_FRAME_NONE depth=KICKOS_X86_64_TRAP_DEPTH_EXITKSW stack=kernel
 * class RET       frame=KICKOS_X86_64_TRAP_NEST depth=KICKOS_X86_64_TRAP_DEPTH_RET
 * class RETSW     frame=KICKOS_X86_64_TRAP_FRAME_NONE depth=KICKOS_X86_64_TRAP_DEPTH_RETSW
 * class IDLE      frame=KICKOS_X86_64_TRAP_NEST depth=KICKOS_X86_64_TRAP_DEPTH_IDLE
 * class PANIC     frame=KICKOS_X86_64_PANIC_FRAME depth=KICKOS_X86_64_PANIC_DEPTH stack=panic
 */

#endif
