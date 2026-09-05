/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent _kickos_int_level1 (arch/xtensa/chip/esp32/startup.S) reserves at the bottom of
 * the interrupted thread's stack.
 *
 * ONE CLASS. LX6 has no MPU and no per-thread kernel block, so every level-1 interrupt runs
 * its dispatch on the stack it interrupted and there is nothing to transfer to. The class is
 * PREEMPT and not a syscall class: a KickOS syscall on this part is a plain call, so the only
 * involuntary descent is the interrupt.
 *
 * THE PANIC TAIL IS EXCLUDED, as it is on rv32imac and armv7m: counting kpanic takes the depth
 * to 608 and the zone to 864, sizing this board's idle stack for a chain that ends in a halt
 * rather than in a resume. RESIDUAL: a panic reached from a level-1 interrupt on a thread at the
 * floor descends past the zone. check_trap_redzone.sh prints the without-exclusions figure on
 * every run so the number cannot go quiet.
 *
 * SPILL_ALL_WINDOWS ADDS NOTHING HERE. It flushes each ancestor frame to that frame's OWN base
 * save area, which sits below the ancestor's sp and therefore ABOVE the interruptee's, so the
 * spill writes inside stack the interruptee already owns.
 */

#ifndef KICKOS_ARCH_LX6_TRAP_STACK_H
#define KICKOS_ARCH_LX6_TRAP_STACK_H

/* The interrupt frame _kickos_int_level1 places below the interruptee sp: the a-registers, the
 * 16 f-registers with FCR/FSR, PC and PS. Must equal F_SIZE in xtensa_frame.h, which the entry
 * subtracts; arch_lx6.cc static_asserts the pair. */
#define KICKOS_LX6_TRAP_FRAME 256

/* What kickos_lx6_dispatch_l1 and everything it reaches descend BELOW that frame, measured by
 * tests/static/check_trap_redzone.sh.
 *
 * PER KERNEL-CORE COUNT. Above one core the deepest descent leaves the scheduler through the
 * kernel lock and keeps going: klock_enter -> arch_kernel_lock -> doorbell_poll ->
 * kickos_lx6_doorbell_service -> kickos_irq_route_service -> arch_irq_mask ->
 * kickos_lx6_hw_mask -> phys_int_disable. At one core arch_kernel_lock is an empty macro
 * (arch/arch.h) and the chain ends at klock_enter.
 *
 * THE LAST FOUR FRAMES ARE CHARGED WHETHER OR NOT AN ASK IS PENDING. Freeze N2 puts the route
 * drain in the doorbell SERVICE BODY, that body is reached from arch_kernel_lock's acquire
 * poll, and the callgraph reader is reachability-based, so every chain that can spin on the
 * kernel lock charges the deepest gating operation. A cross-core action added near a lock path
 * pays the same way.
 *
 * MARGIN: KICKOS_LX6_TRAP_FRAME 256 + 608 = 864 against a KICKOS_MIN_STACK_SIZE of 896, so 32
 * bytes. The next thing that deepens the lx6 interrupt path fails the BUILD in
 * tests/static/check_trap_redzone.sh. The two ways out are shortening the chain named above,
 * whose last three frames are this backend's own gating path and used everywhere, or raising
 * KICKOS_MIN_STACK_SIZE, which is fleet-wide. */
#if KICKOS_KERNEL_CORES > 1
#define KICKOS_LX6_TRAP_DEPTH 608
#else
#define KICKOS_LX6_TRAP_DEPTH 432
#endif

#endif
