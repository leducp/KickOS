/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What the AArch64 entries (switch.S) require of the stacks they build frames on. Included by
 * switch.S and by the linker scripts' cpp pass, so plain integer #defines only: the gate reads
 * each one as an immediate.
 *
 * AN EL1h INTERRUPT RUNS ITS WHOLE DISPATCH ON THE STACK IT INTERRUPTED: idle's, a privileged
 * thread's own, or a death-path stub's kernel block. An EL0 entry masks interrupts until its
 * eret, so nothing nests on a block under IRQK or SYSK.
 *
 * NO INTERRUPT NESTS BELOW A SWITCH FRAME: arch_switch runs under the kernel IrqLock and the
 * incoming side's unlock runs before the eret restores its mask. So each death-path descent is
 * two classes, the interrupt nested at its deepest byte with the switch cut, and the switch
 * with nothing below it.
 *
 * Each figure is the deepest reading over every armv8a preset. Thread-stack figures are rounded
 * up to the next multiple of 64; kernel-block ones stay at the measurement.
 */

#ifndef KICKOS_ARCH_ARMV8A_TRAP_STACK_H
#define KICKOS_ARCH_ARMV8A_TRAP_STACK_H

/* SAVE_FRAME's extent; switch.S subtracts this macro. */
#define KICKOS_ARMV8A_TRAP_FRAME 800

/* The frame term where nothing interrupts: VEC_SLOT pushes nothing, and a class ending in the
 * masked switch has no interrupt below it. */
#define KICKOS_ARMV8A_TRAP_FRAME_NONE 0

/* IRQ and IRQK. 752 on qemu-arm64-amp, 688 benchgicv3, 672 gicv3, 560 benchsmp and benchsmp2,
 * 544 smp and smpiso, 480 amp3, 464 amp2, 432 bench, 384 qemu-arm64 and imx8mp-evk. The AMP
 * doorbell's payload drain wins:
 *   kickos_armv8a_irq[32] -> kickos_armv8a_gic_dispatch[32] -> kickos_arm64_doorbell_service[32]
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

/* The EL0 synchronous entry on the block, the syscall and the fault it contains. 1824 on
 * qemu-arm64-benchgicv3, a spawn seeding the child's tables:
 *   syscall_dispatch[112] -> thread_create_call -> spawn_masked[448] -> thread_create[144]
 *   -> domain_for -> claim_slot -> aspace_image_seed[112] -> arch_aspace_map[112]
 *   -> map_into[112]x3 -> kickos_frame_alloc -> klock_enter -> ... -> wait_gicd_rwp
 *   -> kfault_terminate
 * 1504 on qemu-arm64, a reply-receive whose exit teardown wakes into the switch. */
#define KICKOS_ARMV8A_TRAP_DEPTH_SYSK 1824

/* The fault and slay stubs on the block with an interrupt nested below. 1248 on
 * qemu-arm64-benchgicv3, the space release's rendezvous timing out into the console:
 *   kickos_thread_fault_exit -> exit_current[112] -> cap_teardown[96] -> teardown_entry
 *   -> obj_ref_drop -> domain_release -> drop_space -> aspace_release[96]
 *   -> arch_aspace_destroy -> kickos_arm64_instruction_side_rendezvous -> arch_ipi_wait
 *   -> doorbell::hex1 -> console_tx_insert_line[112] -> console_write_line_sync[80]
 *   -> klock_enter -> ... -> wait_gicd_rwp -> kfault_terminate
 * NEST + 1248 = 2816 is the block's deepest need, against 4092 above the canary. */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITK 1248

/* The same stubs through the switch. 1648 on qemu-arm64-benchgicv3:
 *   kickos_thread_fault_exit -> kprintf_fault[416] -> cap_console_deliver -> sched::wake
 *   -> pick_and_seat -> arch_switch -> kickos_armv8a_switch_now[800] -> kickos_switch_unlock
 *   -> sched_flush_owed -> klock_resched_ask -> kickos_armv8a_gic_doorbell_send[96] */
#define KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW 1648

/* kickos_thread_return on a privileged thread's own stack with an interrupt nested below. 1216
 * on qemu-arm64-benchgicv3, down EXITK's chain. NEST + 1216 = 2784 is what sets
 * KICKOS_MIN_STACK_SIZE. */
#define KICKOS_ARMV8A_TRAP_DEPTH_RET 1216

/* The same return through the switch. 1488 on qemu-arm64-benchgicv3, cap_teardown's
 * mutex_force_unlock waking into kickos_armv8a_switch_now[800]. */
#define KICKOS_ARMV8A_TRAP_DEPTH_RETSW 1536

/* A privileged fault's reporter, kickos_armv8a_exception, on the stack that faulted. 1232 on
 * qemu-arm64-benchgicv3: kprintf[560] into the console and the kernel lock under it. */
#define KICKOS_ARMV8A_TRAP_DEPTH_FAULT 1280

/* idle_entry's own frame, above the interrupt idle waits for: 16 on every preset. */
#define KICKOS_ARMV8A_TRAP_DEPTH_IDLE 64

/* The panic reporter on its own array: 672 on qemu-arm64-benchgicv3, and this is the next
 * multiple of 64 strictly above it. */
#define KICKOS_ARMV8A_PANIC_FRAME 0
#define KICKOS_ARMV8A_PANIC_DEPTH 704

#endif
