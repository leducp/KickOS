/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the AArch64 entries (switch.S) require of the stacks they build frames on. Included by
 * switch.S and by the linker scripts' cpp pass, so plain integer #defines only: the gate reads
 * each one as an immediate.
 *
 * AN EL1h INTERRUPT RUNS ITS WHOLE DISPATCH ON THE STACK IT INTERRUPTED: idle's, a privileged
 * thread's own, or a death-path stub's kernel block. An EL0 entry masks interrupts until its
 * eret but in arch_irq_window, so nothing nests on a block under IRQK, and under SYSK only below
 * that one callee (SYSWIN).
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

/* IRQ and IRQK. 736 on qemu-arm64-amp, 672 benchgicv3 and benchsmp12, 656 gicv3, 560 benchsmp
 * and benchsmp2, 544 smp and smpiso, 464 amp3, 448 amp2, 432 bench, 384 qemu-arm64 and
 * imx8mp-evk. The AMP doorbell's payload drain wins:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[32] -> kickos_arm64_doorbell_service[16]
 *   -> amp::node_service[144] -> amp::take_call[160] -> amp::depth_ok[96] -> amp::send[64]
 *   -> amp::send_on[64] -> arch_ipi_send[32] -> kickos_armv8a_gic_doorbell_send[96]
 * Above one core it is an interrupt event's wake whose switch spins on the kernel lock and
 * services a route ask: ... notify_raise -> sched::wake -> pick_and_seat -> klock_attach
 * -> arch_kernel_lock -> kickos_doorbell_poll -> kickos_arm64_doorbell_service
 * -> kickos_irq_route_service -> arch_irq_mask -> wait_gicd_rwp -> kfault_terminate. */
#define KICKOS_ARMV8A_TRAP_DEPTH_IRQ 768

/* An interrupt's whole extent below the sp it interrupts, FRAME + DEPTH_IRQ, which
 * arch_armv8a.cc asserts: the frame term of every class an interrupt can land under. */
#define KICKOS_ARMV8A_TRAP_NEST 1568

/* The EL0 synchronous entry on the block, the syscall and the fault it contains. 1968 on
 * qemu-arm64-benchgicv3 and 1984 on benchsmp12 under the KickOS toolchain's GCC 16.2, a spawn
 * staging 9 grants and seeding the child's tables:
 *   syscall_dispatch[144] -> thread_create_call[32] -> spawn_masked[560] -> thread_create[144]
 *   -> task_for[32] -> domain_for[64] -> claim_slot[64] -> aspace_image_seed[128]
 *   -> arch_aspace_map[112] -> map_into[112]x3 -> kickos_frame_alloc[48] -> klock_enter
 *   -> ... -> wait_gicd_rwp -> kfault_terminate
 * 1568 on the amp3 nodes, which stage 13 grants, and 1552 on qemu-arm64; arch_armv8a.cc refuses
 * more than 13. Reserved at 2048, above the measurement. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSK 2048

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

/* The fault and slay stubs on the block with an interrupt nested below. 1248 on
 * qemu-arm64-benchgicv3 and benchsmp12, the space release's rendezvous timing out into the console:
 *   kickos_thread_fault_exit -> exit_current[128] -> cap_teardown[96] -> teardown_entry
 *   -> obj_ref_drop -> domain_release -> drop_space -> aspace_release[96]
 *   -> arch_aspace_destroy -> kickos_arm64_instruction_side_rendezvous -> arch_ipi_wait
 *   -> doorbell::hex1 -> console_tx_insert_line[112] -> console_write_line_sync[80]
 *   -> klock_enter -> ... -> wait_gicd_rwp -> kfault_terminate
 * NEST + 2048 = 3616 is the block's deepest need, against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITK 2048

/* The same stubs through the switch. 1616 on qemu-arm64-benchgicv3:
 *   kickos_thread_fault_exit -> kprintf_fault[416] -> cap_console_deliver -> sched::wake
 *   -> pick_and_seat -> arch_switch -> kickos_armv8a_switch_now[800] -> kickos_switch_unlock
 *   -> sched_flush_owed -> klock_resched_ask -> kickos_armv8a_gic_doorbell_send[96] */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW 2048

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below. 1216
 * on qemu-arm64-benchgicv3 and benchsmp12, down EXITK's chain. NEST + 1280 = 2848 is what sets
 * KICKOS_MIN_STACK_SIZE. */
#define KICKOS_ARMV8A_TRAP_DEPTH_RET 1280

/* The same return through the switch. 1520 on qemu-arm64-benchgicv3, cap_teardown's endpoint
 * close waking into kickos_armv8a_switch_now[800]. */
#define KICKOS_ARMV8A_TRAP_DEPTH_RETSW 1792

/* A privileged fault's reporter, kickos_armv8a_exception, on the stack that faulted. 1184 on
 * qemu-arm64-benchgicv3 and benchsmp12: kprintf[560] into the console and the kernel lock under
 * it. */
#define KICKOS_ARMV8A_TRAP_DEPTH_FAULT 2048

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_ARMV8A_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 656 on qemu-arm64-benchgicv3 and benchsmp12, and this
 * is the next multiple of 64 strictly above it. */
#define KICKOS_ARMV8A_PANIC_FRAME 0
#define KICKOS_ARMV8A_PANIC_DEPTH 704

#endif
