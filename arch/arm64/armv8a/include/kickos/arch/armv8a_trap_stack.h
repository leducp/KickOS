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

/* SAVE_FRAME's extent; switch.S subtracts this macro. */
#define KICKOS_ARMV8A_TRAP_FRAME 800

/* The frame term where nothing interrupts: VEC_SLOT pushes nothing, and a class ending in the
 * masked switch has no interrupt below it. */
#define KICKOS_ARMV8A_TRAP_FRAME_NONE 0

/* An EL0 entry writes nothing on the thread's own stack: a lower-EL vector runs on SP_EL1, the
 * thread's kernel block, and ENTER_FROM_EL0 reads the EL0 sp out of SP_EL0. */
#define KICKOS_ARMV8A_TRAP_ENTRY_FRAME 0
#define KICKOS_ARMV8A_TRAP_NEED_USER 0

/* IRQ and IRQK. 752 on qemu-arm64-benchgicv3 and benchsmp12, 720 amp, 704 gicv3, 624 benchsmp,
 * 576 smp, 528 amp2, 432 qemu-arm64 and imx8mp-evk. Above one core it is the console drain's
 * wake, whose switch spins on the kernel lock and services a route ask that GICv3 refuses:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[48] -> kickos_isr_irq[32]
 *   -> console_tx_isr[80] -> console_tx_room_freed[48] -> console_dark_wake[48] -> sched::wake
 *   -> pick_and_seat[80] -> klock_attach[48] -> arch_kernel_lock -> kickos_doorbell_poll
 *   -> kickos_doorbell_service -> kickos_irq_route_service[48] -> arch_irq_mask[48]
 *   -> wait_gicd_rwp[48] -> kpanic_refuse_unlocked[32]
 * On one kernel core the AMP doorbell's payload drain wins:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[32] -> kickos_doorbell_service[32]
 *   -> amp::node_service[144] -> amp::take_reply[128] -> amp::depth_ok[96] -> amp::send[112]
 *   -> kickos_armv8a_gic_doorbell_send[80] */
#define KICKOS_ARMV8A_TRAP_DEPTH_IRQ 768

/* An interrupt's whole extent below the sp it interrupts, FRAME + DEPTH_IRQ, which
 * arch_armv8a.cc asserts: the frame term of every class an interrupt can land under. */
#define KICKOS_ARMV8A_TRAP_NEST 1568

/* The EL0 synchronous entry on the block, the syscall and the fault it contains. 2032 on
 * qemu-arm64-benchgicv3 and benchsmp12 under the KickOS toolchain's GCC 16.2, a refused spawn
 * discarding the task it built, which serves the console, and switching to the writer the death
 * wakes:
 *   syscall_dispatch[144] -> thread_create_call[32] -> spawn_masked[544] -> spawn_unwind[64]
 *   -> task_discard -> free_task[32] -> cap_console_task_ended[32] -> console_on_driver_death[48]
 *   -> console_dark_wake[48] -> sched::wake[32] -> ... -> kickos_armv8a_switch_now[800]
 *   -> ... -> kickos_armv8a_gic_doorbell_send[96]
 * arch_armv8a.cc refuses more than 13 grants. Reserved at 2304, above the measurement. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSK 2304

/* An interrupt nested below arch_irq_window, the one place a system call opens interrupts on the
 * block: the EL0 synchronous entry's frame and an interrupt's whole extent, FRAME + NEST, which
 * arch_armv8a.cc asserts. */
#define KICKOS_ARMV8A_TRAP_WINDOW 2368

/* The system call's descent to arch_irq_window, above SYSWIN's frame. 400 on the benchsmp and
 * benchgicv3 presets, 352 on qemu-arm64:
 *   syscall_dispatch[144] -> presync_prepare[16] -> presync_run[192]
 *   -> GranuleWindows::granule[32] -> arch_irq_window[16]
 * WINDOW + 1024 = 3392 against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSWIN 1024

/* The fault and slay stubs on the block with an interrupt nested below. 1056 on
 * qemu-arm64-benchgicv3 and benchsmp12, the fault report's line waking the publisher it raced:
 *   kickos_thread_fault_exit[48] -> kprintf_fault[368] -> console_emit[80]
 *   -> console_chip_writer_leave[48] -> console_dark_wake[48] -> ... -> klock_attach[48]
 *   -> arch_kernel_lock -> ... -> wait_gicd_rwp[48] -> kpanic_refuse_unlocked[32]
 * NEST + 2048 = 3616 is the block's deepest need, against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITK 2048

/* The same stubs through the switch. 1472 on qemu-arm64-smp:
 *   kickos_thread_fault_exit -> exit_current[112] -> cap_teardown[80] -> teardown_entry[80]
 *   -> obj_close_protocol -> endpoint_rights_dropped -> refuse_senders -> sched::wake
 *   -> pick_and_seat -> arch_switch -> kickos_armv8a_switch_now[800] -> kickos_switch_unlock
 *   -> sched_flush_owed -> klock_resched_ask -> resched_owe */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW 2048

/* A privileged fault's reporter, kickos_armv8a_exception, on idle's stack, the one privileged
 * thread's. 1168 on qemu-arm64-benchgicv3 and benchsmp12: kprintf[560] into the console and the
 * kernel lock under it. */
#define KICKOS_ARMV8A_TRAP_DEPTH_FAULT 2048

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_ARMV8A_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 640 on qemu-arm64-benchgicv3 and benchsmp12, under an
 * enforced 704. */
#define KICKOS_ARMV8A_PANIC_FRAME 0
#define KICKOS_ARMV8A_PANIC_DEPTH 704

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
 */

#endif
