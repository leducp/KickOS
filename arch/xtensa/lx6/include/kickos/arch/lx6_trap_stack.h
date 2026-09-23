/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent _kickos_int_level1 (arch/xtensa/chip/esp32/startup.S) reserves at the bottom of
 * the interrupted thread's stack.
 *
 * One class: LX6 has no MPU and no per-thread kernel block, so every level-1 interrupt runs
 * its dispatch on the stack it interrupted and there is nothing to transfer to. The class is
 * PREEMPT and not a syscall class: a KickOS syscall on this part is a plain call, so the only
 * involuntary descent is the interrupt.
 *
 * The panic tail is off this class structurally: kpanic and kpanic_at reach the reporter only
 * through kickos_panic_stack_enter, which is assembly and has no .ci entry, so the walk cannot
 * pass through it and the console is priced in the PANIC class below instead.
 *
 * SPILL_ALL_WINDOWS adds nothing here: it flushes each ancestor frame to that frame's own base
 * save area, which sits below the ancestor's sp and therefore above the interruptee's, so the
 * spill writes inside stack the interruptee already owns.
 */

#ifndef KICKOS_ARCH_LX6_TRAP_STACK_H
#define KICKOS_ARCH_LX6_TRAP_STACK_H

/* The interrupt frame _kickos_int_level1 places below the interruptee sp: the a-registers, the
 * 16 f-registers with FCR/FSR, PC and PS. Must equal F_SIZE in xtensa_frame.h, which the entry
 * subtracts; arch_lx6.cc static_asserts the pair. */
#define KICKOS_LX6_TRAP_FRAME 256

/* What kickos_lx6_dispatch_l1 and everything it reaches descend below that frame, measured by
 * tests/static/check_trap_redzone.sh.
 *
 * Per kernel-core count, and the two postures win down different chains. Above one core the
 * deepest descent leaves the scheduler through a cross-core ask and keeps going, 608 measured
 * on esp32-wroom-smp: resched_after_wake -> pick_and_seat -> ask -> klock_resched_ask ->
 * arch_ipi_send -> kickos_doorbell_poll -> kickos_lx6_doorbell_service ->
 * kickos_irq_route_service -> arch_irq_mask -> kickos_lx6_hw_mask -> phys_int_disable. At one
 * core arch_kernel_lock is an empty macro (arch/arch.h) and no ask exists, so neither
 * klock_enter nor ask is on any chain and the winner instead runs resched_after_wake ->
 * pick_and_seat -> arch_switch -> xtensa_switch[128], 416 on both one-core presets.
 *
 * The last four frames are charged whether or not an ask is pending: the route drain sits in
 * the doorbell service body, that body is reached from arch_kernel_lock's acquire poll, and the
 * callgraph reader is reachability-based, so every chain that can spin on the kernel lock
 * charges the deepest gating operation. A cross-core action added near a lock path pays the
 * same way.
 *
 * Above one core the enforced figure is the measurement, 608 against 608, and the whole switch
 * path is inside pick_and_seat's own frame: switch_to and switch_book inline into it, so one
 * more value held live across the ask buys that frame another 16 bytes. The maximum is a tie
 * between routes here, the acquire poll and the ask reaching the doorbell service body down
 * three frames of equal cost, so a flat reading after a change is not evidence the change cost
 * nothing.
 *
 * Margin: KICKOS_LX6_TRAP_FRAME 256 + 608 = 864 against a KICKOS_MIN_STACK_SIZE of 896, so 32
 * bytes. At one core there are 16 bytes of give the same way, 416 against 432, that posture
 * compiling none of the ask. The two ways out are shortening the chain named above, whose last
 * three frames are this backend's own gating path and used everywhere, or raising
 * KICKOS_MIN_STACK_SIZE, which is fleet-wide. */
#if KICKOS_KERNEL_CORES > 1
#define KICKOS_LX6_TRAP_DEPTH 608
#else
#define KICKOS_LX6_TRAP_DEPTH 432
#endif

/* The panic reporter's own stack, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed, so PREEMPT measures no console at all and this class is where the
 * console is priced instead.
 *
 * Frame is one trap frame. The entry raises INTLEVEL to 15 before the move, so no interrupt
 * lands here, but a window exception is not maskable and an ISA exception is not either, and
 * both build their frame on the stack in a1. A plain integer because check_trap_redzone.sh
 * scrapes it as an immediate; arch_xtensa.cc asserts it against KICKOS_LX6_TRAP_FRAME.
 *
 * 528 measured on esp32-wroom-smp and 272 on the two one-core presets, under an enforced 768.
 *
 * This arch is the one where the entry is not free: the windowed ABI gives it no way to write
 * the incoming a1 without opening a frame first, so kickos_panic_stack_enter spends 32 bytes on
 * the stack it is leaving; trap_redzone_roots.txt declares that as its unsized cost. */
#define KICKOS_LX6_PANIC_FRAME 256
#define KICKOS_LX6_PANIC_DEPTH 768

#endif
