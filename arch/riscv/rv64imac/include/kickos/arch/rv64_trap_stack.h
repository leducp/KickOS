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
 * DISPATCH ON THE STACK IT INTERRUPTED: idle's, a privileged thread's own, or a death-path
 * stub's kernel block. AN S-MODE EXCEPTION takes the per-hart trap stack.
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

/* KICKOS_RV64_FRAME, the one frame every entry builds; arch_rv64imac.cc asserts the two agree. */
#define KICKOS_RV64_TRAP_FRAME 256

/* The frame term where nothing interrupts: a class ending in the masked switch. */
#define KICKOS_RV64_TRAP_FRAME_NONE 0

/* A U-mode trap writes nothing on the thread's own stack: kickos_rv64_stvec swaps sp for the
 * trap top before its first store and builds the frame on the thread's kernel block. */
#define KICKOS_RV64_TRAP_ENTRY_FRAME 0
#define KICKOS_RV64_TRAP_NEED_USER 0

/* IRQ and IRQK. 880 on qemu-riscv64-benchsmp and -benchsmp2, 816 smp, 624 bench, 592
 * qemu-riscv64 and sv48. The unexpected-cause report, whose console spins on the kernel lock
 * and services a route ask:
 *   kickos_rv64_isr_dispatch[48] -> kprintf[352] -> console_emit[96]
 *   -> console_tx_insert_line[96] -> console_write_line_sync[64] -> klock_enter[32]
 *   -> arch_kernel_lock[32] -> kickos_doorbell_poll[32] -> kickos_rv64_doorbell_service[48]
 *   -> kickos_irq_route_service[64] -> arch_irq_unmask[16] */
#define KICKOS_RV64_TRAP_DEPTH_IRQ 896

/* An interrupt's whole extent below the sp it interrupts, FRAME + DEPTH_IRQ, which
 * arch_rv64imac.cc asserts: the frame term of every class an interrupt can land under. */
#define KICKOS_RV64_TRAP_NEST 1152

/* The U-mode ecall and the U-mode fault on the block. 2016 on qemu-riscv64-benchsmp and
 * -benchsmp2, a spawn staging 9 grants and seeding the child's tables:
 *   syscall_dispatch[128] -> thread_create_call[32] -> spawn_masked[576] -> thread_create[144]
 *   -> task_for[32] -> domain_for[64] -> claim_slot[64] -> aspace_image_seed[160]
 *   -> arch_aspace_map[80] -> map_into[112]x4 -> kickos_frame_alloc[48] -> klock_enter
 *   -> ... -> arch_irq_unmask
 * 1792 on qemu-riscv64 and sv48. arch_rv64imac.cc refuses more than 9 grants. Reserved at 2304,
 * above the measurement. */
#define KICKOS_RV64_TRAP_DEPTH_SYSK 2304

/* An interrupt nested below arch_irq_window, the one place a system call opens interrupts on the
 * block: the U-mode entry's frame and an interrupt's whole extent, FRAME + NEST, which
 * arch_rv64imac.cc asserts. */
#define KICKOS_RV64_TRAP_WINDOW 1408

/* The system call's descent to arch_irq_window, above SYSWIN's frame. 352 on
 * qemu-riscv64-bench, -benchsmp and -benchsmp2, 320 on qemu-riscv64, sv48 and smp:
 *   syscall_dispatch[144] -> presync_prepare[16] -> presync_run[192] -> arch_irq_window[0]
 * WINDOW + 1024 = 2432 against 4092 above the canary. */
#define KICKOS_RV64_TRAP_DEPTH_SYSWIN 1024

/* The per-hart trap stack: an S-mode exception's reporter, and the U-mode entry that found no
 * block. 896 on qemu-riscv64-benchsmp and -benchsmp2, down IRQ's console tail from
 * kickos_rv64_fault_report[64]. */
#define KICKOS_RV64_TRAP_NESTED_DEPTH 960

/* The fault and slay stubs on the block with an interrupt nested below. 1136 on
 * qemu-riscv64-benchsmp and -benchsmp2, the space release freeing the child's tables:
 *   kickos_thread_fault_exit[32] -> exit_current[128] -> cap_teardown[80] -> teardown_entry[64]
 *   -> obj_ref_drop[48] -> domain_release[16] -> drop_space[32] -> aspace_release[96]
 *   -> arch_aspace_destroy[48] -> free_subtree[80]x4 -> kickos_frame_free[48] -> klock_enter
 *   -> ... -> arch_irq_unmask
 * Reserved at 2048, as SYSK is; NEST + 2048 = 3200 is the block's deepest need. */
#define KICKOS_RV64_TRAP_DEPTH_EXITK 2048

/* The same stubs through the switch: 1136 on the same two presets, down the same chain, and
 * reserved at 2048. */
#define KICKOS_RV64_TRAP_DEPTH_EXITKSW 2048

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below. 1104
 * on qemu-riscv64-benchsmp and -benchsmp2, down EXITK's chain from kickos_thread_return[16].
 * NEST + 1152 = 2304 is what sets KICKOS_MIN_STACK_SIZE. */
#define KICKOS_RV64_TRAP_DEPTH_RET 1152

/* The same return through the switch: 1104, down the same chain. */
#define KICKOS_RV64_TRAP_DEPTH_RETSW 1152

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_RV64_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 480 on qemu-riscv64-benchsmp and -benchsmp2, the
 * console's kernel-lock tail under kickos_panic_report[16] -> kputs[16]. */
#define KICKOS_RV64_PANIC_FRAME 0
#define KICKOS_RV64_PANIC_DEPTH 576

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class IRQ     frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_IRQ
 * class IRQK    frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_IRQ stack=kernel
 * class SYSK    frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_DEPTH_SYSK stack=kernel
 * class SYSWIN  frame=KICKOS_RV64_TRAP_WINDOW depth=KICKOS_RV64_TRAP_DEPTH_SYSWIN stack=kernel at=arch_irq_window
 * class NESTED  frame=KICKOS_RV64_TRAP_FRAME depth=KICKOS_RV64_TRAP_NESTED_DEPTH stack=trap
 * class EXITK   frame=KICKOS_RV64_TRAP_NEST depth=KICKOS_RV64_TRAP_DEPTH_EXITK stack=kernel
 * class EXITKSW frame=KICKOS_RV64_TRAP_FRAME_NONE depth=KICKOS_RV64_TRAP_DEPTH_EXITKSW stack=kernel
 * class RET     frame=KICKOS_RV64_TRAP_NEST depth=KICKOS_RV64_TRAP_DEPTH_RET
 * class RETSW   frame=KICKOS_RV64_TRAP_FRAME_NONE depth=KICKOS_RV64_TRAP_DEPTH_RETSW
 * class IDLE    frame=KICKOS_RV64_TRAP_NEST depth=KICKOS_RV64_TRAP_DEPTH_IDLE
 * class PANIC   frame=KICKOS_RV64_PANIC_FRAME depth=KICKOS_RV64_PANIC_DEPTH stack=panic
 */

#endif
