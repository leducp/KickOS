/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent _kickos_int_level1 (arch/xtensa/chip/esp32/startup.S) reserves at the bottom of
 * the interrupted thread's stack.
 *
 * LX6 has no MPU and no per-thread kernel block, so every level-1 interrupt runs its dispatch
 * on the stack it interrupted, and a KickOS syscall here is a plain call: the interrupt is the
 * only involuntary descent.
 *
 * SPILL_ALL_WINDOWS adds nothing: it flushes each ancestor frame to that frame's own base save
 * area, which sits above the interruptee's sp.
 */

#ifndef KICKOS_ARCH_LX6_TRAP_STACK_H
#define KICKOS_ARCH_LX6_TRAP_STACK_H

/* The interrupt frame: the a-registers, the 16 f-registers with FCR/FSR, PC and PS. Must equal
 * F_SIZE in xtensa_frame.h, which the entry subtracts; arch_xtensa.cc asserts the pair. */
#define KICKOS_LX6_TRAP_FRAME 256

/* What kickos_lx6_dispatch_l1 descends below that frame, per kernel-core count.
 *
 * Above one core, 576 on esp32-wroom-benchsmp and 560 on esp32-wroom-smp, a wake whose switch
 * re-acquires the kernel lock: irq_event_isr -> notify_raise -> wake -> resched_after_wake
 * -> pick_and_seat -> klock_attach -> arch_kernel_lock -> kickos_doorbell_poll
 * -> kickos_lx6_doorbell_service -> kickos_irq_route_service -> arch_irq_mask
 * -> kickos_lx6_hw_mask -> phys_int_disable.
 * The poll's four frames are charged to every chain that can spin on the kernel lock, pending
 * drain or not. switch_to and switch_book inline into pick_and_seat, so one more value held live
 * across the switch costs that frame 16 bytes.
 *
 * At one core, 416 on the three one-core presets: resched_after_wake -> pick_and_seat
 * -> arch_switch -> xtensa_switch[128].
 *
 * FRAME + 576 = 832 against a KICKOS_MIN_STACK_SIZE of 896. */
#if KICKOS_KERNEL_CORES > 1
#define KICKOS_LX6_TRAP_DEPTH 576
#else
#define KICKOS_LX6_TRAP_DEPTH 432
#endif

/* The panic reporter's own stack. Frame is one trap frame: the entry raises INTLEVEL to 15
 * before the move, but a window exception and an ISA exception are not maskable and both build
 * their frame on the stack in a1. arch_xtensa.cc asserts it against KICKOS_LX6_TRAP_FRAME.
 *
 * 528 on esp32-wroom-benchsmp, 512 on esp32-wroom-smp, 272 on the one-core presets.
 * kickos_panic_stack_enter spends 32 bytes on the stack it leaves, the windowed ABI giving it
 * no way to write a1 without opening a frame; trap_redzone_roots.txt declares that cost. */
#define KICKOS_LX6_PANIC_FRAME 256
#define KICKOS_LX6_PANIC_DEPTH 768

/* idle_entry and arch_idle_wait, whose frames sit above the level-1 interrupt idle waits in:
 * 64 on every registered preset, left at the measurement as PREEMPT is. */
#define KICKOS_LX6_TRAP_DEPTH_IDLE 64

#endif
