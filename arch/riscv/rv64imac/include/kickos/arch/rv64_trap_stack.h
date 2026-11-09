/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the RV64IMAC supervisor entries (switch.S) require of the stacks they build frames on.
 * Included by rv64_frame.h, and so by switch.S and startup.S, so plain integer #defines only:
 * the gate reads each one as an immediate.
 *
 * A U-MODE TRAP BUILDS ITS FRAME ON THE THREAD'S KERNEL BLOCK and runs its dispatch there with
 * SIE clear, but for arch_irq_window, where a system call lets an interrupt nest on the block
 * (SYSWIN); nothing nests under IRQK or SYSK otherwise. AN S-MODE INTERRUPT RUNS ITS WHOLE
 * DISPATCH ON THE STACK IT INTERRUPTED: idle's or a death-path stub's kernel block. AN S-MODE
 * EXCEPTION takes the per-hart trap stack.
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock. So each
 * death-path descent is two classes, the interrupt nested at its deepest byte with the switch
 * cut, and the switch with nothing below it.
 *
 * Each figure is the deepest reading over every rv64imac preset. Thread-stack figures are
 * rounded up to the next multiple of 64, kernel-block ones reserved at a round figure above the
 * measurement, and the two arrays sized once per hart or per image take the next multiple of 64
 * strictly above.
 * The page-table walks are charged four activations, sv48's, on the three-level presets too.
 */

#ifndef KICKOS_ARCH_RV64_TRAP_STACK_H
#define KICKOS_ARCH_RV64_TRAP_STACK_H

/* The image's startup on an unprivileged thread's OWN stack, which any thread of a space may
 * carry (kickos_task_start): the unwind-table registration, the toolchain runtime's constructors
 * and their malloc and libc-lock calls. 360 on every qemu-riscv64 preset's cxxtest, read off a
 * painted stack the least a spawn admits, entry included. Reserved at 512;
 * KICKOS_MIN_STACK_SIZE must hold it. An app's own deeper constructors are not in it. */
#define KICKOS_RV64_TASK_START 512

/* KICKOS_RV64_FRAME, the one frame every entry builds; arch_rv64imac.cc asserts the two agree. */
#define KICKOS_RV64_TRAP_FRAME 256

/* The frame term where nothing interrupts: a class ending in the masked switch. */
#define KICKOS_RV64_TRAP_FRAME_NONE 0

/* A U-mode trap writes nothing on the thread's own stack: kickos_rv64_stvec swaps sp for the
 * trap top before its first store and builds the frame on the thread's kernel block. */
#define KICKOS_RV64_TRAP_ENTRY_FRAME 0
#define KICKOS_RV64_TRAP_NEED_USER 0

/* IRQ and IRQK. 992 on qemu-riscv64-benchsmp and -benchsmp2, 880 smp, 816 bench, 768
 * qemu-riscv64 and sv48. The unexpected-cause report, whose line ends its chip-writer bracket by
 * waking the publisher it raced, the wake seating a thread whose reent state is primed through
 * the space's tables:
 *   kickos_rv64_isr_dispatch[48] -> kprintf[352] -> console_emit[96]
 *   -> console_chip_writer_leave[32] -> console_dark_wake[32] -> wake[16]
 *   -> resched_after_wake[16] -> pick_and_seat[80] -> reent_prime[48] -> kaccess_to_user[0]
 *   -> access_copy[96] -> arch_aspace_acquire[96] -> walk_leaf[80] */
#define KICKOS_RV64_TRAP_DEPTH_IRQ 1024

/* An interrupt's whole extent below the sp it interrupts, FRAME + DEPTH_IRQ, which
 * arch_rv64imac.cc asserts: the frame term of every class an interrupt can land under. */
#define KICKOS_RV64_TRAP_NEST 1280

/* The U-mode ecall and the U-mode fault on the block. 2176 on qemu-riscv64-benchsmp and
 * -benchsmp2, a task create seeding the child's tables:
 *   syscall_dispatch[176] -> task_create_call[256] -> spawn_locked[448] -> thread_create[144]
 *   -> task_for[64] -> domain_for[64] -> claim_slot[80] -> aspace_image_seed[144]
 *   -> arch_aspace_map[96] -> map_into[112]x4 -> kickos_frame_alloc[48] -> klock_enter[32]
 *   -> ... -> arch_irq_unmask[16]
 * 2048 on qemu-riscv64. arch_rv64imac.cc refuses more than 9 grants. Reserved at 2304, above
 * the measurement. */
#define KICKOS_RV64_TRAP_DEPTH_SYSK 2304

/* An interrupt nested below arch_irq_window, the one place a system call opens interrupts on the
 * block: the U-mode entry's frame and an interrupt's whole extent, FRAME + NEST, which
 * arch_rv64imac.cc asserts. */
#define KICKOS_RV64_TRAP_WINDOW 1536

/* The system call's descent to arch_irq_window, above SYSWIN's frame. 384 on
 * qemu-riscv64-bench, -benchsmp and -benchsmp2, 336 on qemu-riscv64:
 *   syscall_dispatch[160] -> frame_create_call[96] -> presync_run[80] -> arch_irq_window[0]
 * WINDOW + 1024 = 2560 against 4092 above the canary. */
#define KICKOS_RV64_TRAP_DEPTH_SYSWIN 1024

/* The per-hart trap stack: an S-mode exception's reporter, and the U-mode entry that found no
 * block. 896 on qemu-riscv64-benchsmp and -benchsmp2, down IRQ's console tail from
 * kickos_rv64_fault_report[64]. */
#define KICKOS_RV64_TRAP_NESTED_DEPTH 960

/* The fault and slay stubs on the block with an interrupt nested below. 1024 on
 * qemu-riscv64-benchsmp and -benchsmp2, the space release freeing the child's tables:
 *   kickos_thread_fault_exit[32] -> exit_current[128] -> aspace_window_unmap_holder[96]
 *   -> domain_release[16] -> drop_space[32] -> aspace_release[96] -> arch_aspace_destroy[48]
 *   -> free_subtree[80]x4 -> kickos_frame_free[48] -> klock_enter -> ... -> arch_irq_unmask
 * Reserved at 2048, as SYSK is; NEST + 2048 = 3328 is the block's deepest need. */
#define KICKOS_RV64_TRAP_DEPTH_EXITK 2048

/* The same stubs through the switch: 1024 on the same two presets, down the same chain, and
 * reserved at 2048. */
#define KICKOS_RV64_TRAP_DEPTH_EXITKSW 2048

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_RV64_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 480 on qemu-riscv64-benchsmp and -benchsmp2, the
 * console's kernel-lock tail under kickos_panic_report[16] -> kputs[16]. */
#define KICKOS_RV64_PANIC_FRAME 0
#define KICKOS_RV64_PANIC_DEPTH 576

/* THE BOOT STACKS, one KICKOS_RV64_HART_STACK_STRIDE slice of the boot reservation per hart,
 * which virt_rv64.ld asserts holds both classes. DEPTH is the deepest root of each level the roots
 * file declares, summed.
 *
 * MAIN, hart 0's boot, on which nothing nests: SIE stays clear to the first sret and an S-mode
 * exception takes the trap stack. 1616 on the three multicore presets, kmain's idle_build
 * creating idle's thread, 1424 on qemu-riscv64-bench and 1376 on qemu-riscv64.
 *
 * CORE, a secondary's park with the doorbell interrupt nested on it, under the interrupt's
 * frame: 2016 on qemu-riscv64-benchsmp and -benchsmp2, 1920 on -smp, the hart's seating print
 * and the unexpected-cause report each waking a console writer.
 *
 * Both are reserved at 3072. */
#define KICKOS_RV64_MAIN_DEPTH 3072
#define KICKOS_RV64_CORE_DEPTH 3072

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class IRQ     frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_IRQ stack=idle
 * class IRQK    frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_IRQ stack=kernel
 * class SYSK    frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_SYSK stack=kernel
 * class SYSWIN  frame=KICKOS_RV64_TRAP_WINDOW depth=KICKOS_RV64_TRAP_DEPTH_SYSWIN stack=kernel at=arch_irq_window
 * class NESTED  frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_NESTED_DEPTH stack=trap
 * class EXITK   frame=KICKOS_RV64_TRAP_NEST depth=KICKOS_RV64_TRAP_DEPTH_EXITK stack=kernel
 * class EXITKSW frame=KICKOS_RV64_TRAP_FRAME_NONE depth=KICKOS_RV64_TRAP_DEPTH_EXITKSW stack=kernel
 * class IDLE    frame=KICKOS_RV64_TRAP_NEST depth=KICKOS_RV64_TRAP_DEPTH_IDLE stack=idle
 * class PANIC   frame=KICKOS_RV64_PANIC_FRAME depth=KICKOS_RV64_PANIC_DEPTH stack=panic
 * class MAIN    frame=KICKOS_RV64_TRAP_FRAME_NONE depth=KICKOS_RV64_MAIN_DEPTH stack=main
 * class CORE    frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_CORE_DEPTH stack=main core=secondary
 */

#endif
