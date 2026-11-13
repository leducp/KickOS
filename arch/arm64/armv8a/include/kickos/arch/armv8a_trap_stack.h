/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the AArch64 entries (switch.S) require of the stacks they build frames on. Included by
 * switch.S and by the linker scripts' cpp pass, so plain integer #defines only: the gate reads
 * each one as an immediate.
 *
 * AN EL1h INTERRUPT RUNS ITS WHOLE DISPATCH ON THE STACK IT INTERRUPTED: idle's or a death-path
 * stub's kernel block. An EL0 entry masks interrupts until its eret but in arch_irq_window, so
 * nothing nests on a block under IRQK, and under SYSK only below that one callee (SYSWIN).
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock and the
 * incoming side's unlock runs before the eret restores its mask. So each death-path descent is
 * two classes, the interrupt nested at its deepest byte with the switch cut, and the switch
 * with nothing below it.
 *
 * Each figure is the deepest reading over every armv8a preset, and each class is reserved at a
 * round figure above it.
 */

#ifndef KICKOS_ARCH_ARMV8A_TRAP_STACK_H
#define KICKOS_ARCH_ARMV8A_TRAP_STACK_H

#include <kickos/arch/core_stack.ld.h>

/* The image's startup on an unprivileged thread's OWN stack, which any thread of a space may
 * carry (kickos_task_start): the unwind-table registration, the toolchain runtime's constructors
 * and their malloc and libc-lock calls. 400 on every qemu-arm64 preset's cxxtest, read off a
 * painted stack the least a spawn admits, entry included. Reserved at 512;
 * KICKOS_MIN_STACK_SIZE must hold it. An app's own deeper constructors are not in it. */
#define KICKOS_ARMV8A_TASK_START 512

/* SAVE_FRAME's extent; switch.S subtracts this macro. */
#define KICKOS_ARMV8A_TRAP_FRAME 800

/* The frame term where nothing interrupts: VEC_SLOT pushes nothing, and a class ending in the
 * masked switch has no interrupt below it. */
#define KICKOS_ARMV8A_TRAP_FRAME_NONE 0

/* An EL0 entry writes nothing on the thread's own stack: a lower-EL vector runs on SP_EL1, the
 * thread's kernel block, and ENTER_FROM_EL0 reads the EL0 sp out of SP_EL0. */
#define KICKOS_ARMV8A_TRAP_ENTRY_FRAME 0
#define KICKOS_ARMV8A_TRAP_NEED_USER 0

/* IRQ and IRQK. 736 on qemu-arm64-gicv3 and smp12, 720 amp, 624 smp, smp2, smpiso and a710,
 * 592 amp3-n1, 576 amp3-n0 and amp3-n2, 560 amp2, qemu-arm64 and imx8mp-evk. Above one core it
 * is the console drain's wake, whose switch spins on the kernel lock and services a route ask
 * that GICv3 refuses:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[48] -> kickos_isr_irq[48]
 *   -> console_tx_isr[64] -> console_tx_room_freed[32] -> console_dark_wake[48] -> sched::wake
 *   -> pick_and_seat[96] -> klock_attach[48] -> arch_kernel_lock -> kickos_doorbell_poll
 *   -> kickos_doorbell_service -> kickos_irq_route_service[48] -> arch_irq_mask[48]
 *   -> wait_gicd_rwp[48] -> kpanic_refuse_unlocked[32]
 * On one kernel core the AMP doorbell's payload drain wins:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[32] -> kickos_doorbell_service[32]
 *   -> amp::node_service[144] -> amp::take_reply[128] -> amp::depth_ok[96] -> amp::send[112]
 *   -> kickos_armv8a_gic_doorbell_send[80] */
#define KICKOS_ARMV8A_TRAP_DEPTH_IRQ 832

/* An interrupt's whole extent below the sp it interrupts, FRAME + DEPTH_IRQ, which
 * arch_armv8a.cc asserts: the frame term of every class an interrupt can land under. */
#define KICKOS_ARMV8A_TRAP_NEST 1632

/* The EL0 synchronous entry on the block, the syscall and the fault it contains. 2016 on
 * qemu-arm64-gicv3 and smp12 under the KickOS toolchain's GCC 16.2, a task create seeding the
 * child's tables, whose frame allocation spins on the kernel lock and services a route ask
 * that GICv3 refuses; 1776 on qemu-arm64:
 *   syscall_dispatch[176] -> task_create_call[224] -> spawn_locked[448] -> thread_create[128]
 *   -> ... -> aspace_image_seed[96] -> arch_aspace_map[112] -> map_into[96]x3
 *   -> kickos_frame_alloc[48] -> klock_enter[32] -> arch_kernel_lock -> ...
 *   -> wait_gicd_rwp[48] -> kpanic_refuse_unlocked[32]
 * arch_armv8a.cc refuses more than 13 grants. Reserved at 2304, above the measurement. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSK 2304

/* An interrupt nested below arch_irq_window, the one place a system call opens interrupts on the
 * block: the EL0 synchronous entry's frame and an interrupt's whole extent, FRAME + NEST, which
 * arch_armv8a.cc asserts. */
#define KICKOS_ARMV8A_TRAP_WINDOW 2432

/* The system call's descent to arch_irq_window, above SYSWIN's frame. 352 on every armv8a
 * preset:
 *   syscall_dispatch[176] -> frame_create_call[96] -> presync_run[80] -> arch_irq_window[0]
 * WINDOW + 1024 = 3456 against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSWIN 1024

/* The fault and slay stubs on the block with an interrupt nested below. 1232 on
 * qemu-arm64-gicv3 and smp12, the fault report's line waking the publisher it raced:
 *   kickos_thread_fault_exit[32] -> fault_record_print[96] -> kprintf_record[320]
 *   -> kvprintf_fault[176] -> console_emit[64] -> console_chip_writer_leave[32]
 *   -> console_dark_wake[48] -> ...
 *   -> klock_attach[48] -> arch_kernel_lock -> ... -> wait_gicd_rwp[48]
 *   -> kpanic_refuse_unlocked[32]
 * NEST + 2048 = 3680 is the block's deepest need, against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITK 2048

/* The same stubs through the switch. 1856 on qemu-arm64-gicv3 and smp12, 1808 on
 * qemu-arm64-smp:
 *   kickos_thread_fault_exit -> fault_record_print[96] -> kprintf_record[320]
 *   -> kvprintf_fault[176] -> console_emit
 *   -> console_chip_writer_leave -> console_dark_wake -> sched::wake -> pick_and_seat
 *   -> arch_switch -> kickos_armv8a_switch_now[800] -> kickos_switch_unlock
 *   -> sched_flush_owed -> klock_resched_ask -> resched_owe */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW 2048

/* A privileged fault's reporter, kickos_armv8a_exception, on idle's stack, the one privileged
 * thread's. 1824 on qemu-arm64-gicv3 and smp12: kprintf[528] into the console, whose wake
 * switches to the woken writer and rings its core's doorbell. */
#define KICKOS_ARMV8A_TRAP_DEPTH_FAULT 2048

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_ARMV8A_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 624 on qemu-arm64-gicv3, smp12 and the amp2 and amp3
 * nodes, under an enforced 704. */
#define KICKOS_ARMV8A_PANIC_FRAME 0
#define KICKOS_ARMV8A_PANIC_DEPTH 704

/* THE BOOT STACKS, one KICKOS_ARMV8A_CORE_STACK block per core: the primary's below _estack, each
 * secondary's a g_core_stack row. DEPTH is the deepest root of each level the roots file declares,
 * summed.
 *
 * MAIN, the primary's boot with a fault nested on it, no interrupt opening before the first eret:
 * 4128 on qemu-arm64-gicv3 and smp12, 3680 on qemu-arm64 and imx8mp-evk. Its frame is 0,
 * the EL1h synchronous slot branching to the reporter without a push.
 *
 * CORE, a secondary's park with the doorbell interrupt and then a fault nested on it: 4112 on
 * qemu-arm64-amp, whose park drains the AMP node, 3648 on qemu-arm64-gicv3 and smp12. Its frame is
 * the interrupt's SAVE_FRAME.
 *
 * Both are reserved at 5120, which arch_armv8a.cc asserts the block holds. */
#define KICKOS_ARMV8A_MAIN_DEPTH 5120
#define KICKOS_ARMV8A_CORE_DEPTH 5120

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class IRQ     frame=KICKOS_ARMV8A_TRAP_FRAME depth=KICKOS_ARMV8A_TRAP_DEPTH_IRQ stack=idle
 * class IRQK    frame=KICKOS_ARMV8A_TRAP_FRAME depth=KICKOS_ARMV8A_TRAP_DEPTH_IRQ stack=kernel
 * class SYSK    frame=KICKOS_ARMV8A_TRAP_FRAME depth=KICKOS_ARMV8A_TRAP_DEPTH_SYSK stack=kernel
 * class SYSWIN  frame=KICKOS_ARMV8A_TRAP_WINDOW depth=KICKOS_ARMV8A_TRAP_DEPTH_SYSWIN stack=kernel at=arch_irq_window
 * class EXITK   frame=KICKOS_ARMV8A_TRAP_NEST depth=KICKOS_ARMV8A_TRAP_DEPTH_EXITK stack=kernel
 * class EXITKSW frame=KICKOS_ARMV8A_TRAP_FRAME_NONE depth=KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW stack=kernel
 * class FAULT   frame=KICKOS_ARMV8A_TRAP_FRAME_NONE depth=KICKOS_ARMV8A_TRAP_DEPTH_FAULT stack=idle
 * class IDLE    frame=KICKOS_ARMV8A_TRAP_NEST depth=KICKOS_ARMV8A_TRAP_DEPTH_IDLE stack=idle
 * class PANIC   frame=KICKOS_ARMV8A_PANIC_FRAME depth=KICKOS_ARMV8A_PANIC_DEPTH stack=panic
 * class MAIN    frame=KICKOS_ARMV8A_TRAP_FRAME_NONE depth=KICKOS_ARMV8A_MAIN_DEPTH stack=main size=KICKOS_ARMV8A_CORE_STACK
 * class CORE    frame=KICKOS_ARMV8A_TRAP_FRAME depth=KICKOS_ARMV8A_CORE_DEPTH stack=main size=KICKOS_ARMV8A_CORE_STACK core=secondary
 */

#endif
