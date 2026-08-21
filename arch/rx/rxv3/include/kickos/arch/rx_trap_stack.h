/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent the two RXv3 USP frame builders (switch.S) reserve at the BOTTOM of a thread's
 * stack before they agree to build a frame on it.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. kickos_rx_pendsw and kickos_rx_syscall_trap both
 * validate the live USP and then build DOWNWARD from it, and the kernel's C dispatch then
 * runs below that on the same USP. All of it runs SUPERVISOR, and the RX MPU checks user mode
 * only (RX72M UM sec.17.1.1), so supervisor bypasses it entirely. A test that only asked
 * whether the USP lies inside [stack_lo, stack_hi) would let a user thread park the USP one
 * word above stack_lo and have the kernel write supervisor, at an address the thread chose,
 * into whatever sits below: with no padding between pool stacks that is the NEIGHBOUR's saved
 * frame, and the rte that resumes the neighbour pops PC then PSW from it, so clearing PSW.PM
 * there hands the neighbour supervisor. So the bound is on the room REMAINING below the USP,
 * and it covers the frame PLUS the deepest kernel descent that entry point can reach.
 *
 * tests/static/check_trap_redzone.sh re-measures the depth figures below and fails if a worst
 * case exceeds what they reserve.
 *
 * RX INTERRUPTS are accepted on the ISP: acceptance clears PSW.U whatever stack was
 * interrupted, so an interrupt frame lands on the ISP and never on a thread-chosen stack. The
 * three classes below are therefore the three SOFTWARE entry points, and they are all of the
 * sites that build on a USP.
 */

#ifndef KICKOS_ARCH_RX_TRAP_STACK_H
#define KICKOS_ARCH_RX_TRAP_STACK_H

/* Bytes each entry point writes below the USP it arrived with, counted off the EMITTED code
 * (rx-elf-objdump on a linked image, -misa=v3 -mdfpu):
 *
 *   pendsw / the syscall trap's fastpath arm, the same frame in both:
 *     sub #12 (the [R15][PC][PSW] head)               12
 *     pushm r1-r14                            56 ->   68
 *     push.l x7 (FPSW, A1 gu/hi/lo, A0 gu/hi/lo)      96
 *     dpushm.d dr0-dr15                      128 ->  224
 *     dpushm.l dpsw-decnt                     12 ->  236
 *   switch.S checks this itself: FRAME_R1_OFF (168, the `add #FRAME_R1_OFF, r1` the fastpath
 *   arm emits) plus the 68 above r1 is the 236 below.
 *
 *   the syscall trap's generic arm:
 *     sub #8 ([userPC][userPSW] stashed for svc_trampoline's rte)   8
 *     svc_trampoline's own `sub #4, r0` (the 5th-arg slot)   4 ->  12
 *   and syscall_dispatch then runs below THAT.
 *
 * A DFPU-less build writes 96 rather than 236, so these figures are the -mdfpu worst case and
 * stay correct when the DPFPU bank is absent. */
#define KICKOS_RX_TRAP_FRAME_PENDSW 236
#define KICKOS_RX_TRAP_FRAME_SYS 12
#define KICKOS_RX_TRAP_FRAME_SYS_FAST 236

/* Worst-case bytes the kernel's C dispatch descends BELOW each of those, measured on
 * rx72m-st (MinSizeRel, -fcallgraph-info=su,da) as the longest weighted path over the
 * merged call graph from the roots switch.S branches to. On rx-elf the compiler's reported
 * frame ALREADY includes the incoming return-address slot, so the chains below are not
 * missing a per-level 4.
 *
 *   _PENDSW      32  kickos_arch_mpu_commit[28] -> arch_irq_restore[4], the only C the
 *                    restore epilogue runs below an incoming frame. It is the INCOMING
 *                    thread's stack that pays it, and that thread's USP was checked
 *                    against this same figure when it was switched out.
 *   _SYS        664  syscall_dispatch[36] -> syscall_body[108] -> thread_spawn[244]
 *                    -> thread_create[76] -> task_for[32] -> domain_for[40]
 *                    -> grant_region_admissible[32] -> grant_hits_reserved[92]
 *                    -> arch_bitband_present[4]
 *   _SYS_FAST   164  kickos_ipc_fastpath[44] -> sched::switch_prepare[8] -> the indirect
 *                    call at sched.cc:43:42 -> policy_on_switch_in[24] -> arm_slice[20]
 *                    -> ktime_now[4] -> arch_clock_now[32] -> __divdi3[32]
 *
 * ALL THREE EXCLUDE the noreturn kpanic tail, which every KICKOS_ASSERT in the dispatch
 * reaches (kpanic -> kputs -> kconsole_write_impl[152] -> console_emit -> arch_console_write
 * -> console_tx_write[32] -> drain_sync -> wait_slot). Including it takes _SYS to 760 and
 * _SYS_FAST to 428, which the RX floor cannot hold. The residual that leaves: a kernel
 * assertion firing while a thread is parked at the very bottom of its red zone has the
 * console writer descend up to 92 bytes below stack_lo, supervisor.
 * check_trap_redzone.sh prints both figures.
 *
 * ENFORCED figures are those measurements rounded up: 32 -> 64, 664 -> 692, 164 -> 192.
 * A re-measurement MUST carry this board's -misa=v3 -mdfpu baseline; at the compiler's
 * default -misa=v1 with no DFPU every figure here comes out smaller than the truth. The gate
 * fails when a measurement EXCEEDS its figure, so the slack cannot be spent silently. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW 64
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYS 692
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYS_FAST 192

/* What each guard enforces: room below the USP, in bytes.
 *
 * The syscall figure is ONE constant covering both arms: the guard sits at the top of
 * kickos_rx_syscall_trap, above the `cmp #56, r1` that chooses the arm, so it runs before the
 * arm is known. It is a literal that DOMINATES both arms, and arch_rxv3.cc static_asserts the
 * domination in both directions, so a component that grows past it breaks the build.
 *
 * THE SYSCALL FIGURE ALSO CARRIES A WHOLE PENDSW ZONE, and that is the difference between
 * 704 and 1004. Interrupt ACCEPTANCE lands on the ISP, but kickos_rx_pendsw then rebuilds
 * its context save on the USP by hand (mvfc usp / sub #12 / mvtc usp / setpsw u / pushm),
 * and it does that below wherever the dispatch it interrupted had already descended. So a
 * timer ISR pending SWINT over a running syscall composes the two zones on one stack:
 *
 *   12 (the generic arm's frame) + 692 (its descent) + 300 (a PENDSW zone) = 1004
 *
 * RESERVED RATHER THAN ARGUED DOWN. At the point the 692 is actually reached the dispatch
 * holds thread_spawn's function-scope IrqLock, which raises PSW.IPL to 12 and so masks both
 * the timer (IPL 4) and SWINT (IPL 1), making that exact composition unreachable. What is
 * NOT measured is the deepest point reachable with no dispatch lock held: the blocking
 * send/recv/join handlers take none, and this gate measures depth without regard to
 * locking. 692 bounds every path, locked or not, so the sum above bounds the compound case
 * whichever way that question falls, and per-thread kernel stacks delete the arithmetic
 * rather than refine it. */
#define KICKOS_RX_TRAP_REDZONE_PENDSW \
    (KICKOS_RX_TRAP_FRAME_PENDSW + KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW)
#define KICKOS_RX_TRAP_REDZONE_SYS 1004

/* Alignment the guards require of a live USP. Every store in both frame builds is a word
 * store (mov.l, push.l, pushm, and the DPFPU bank on top of them), and arch_context_init
 * aligns the stack top to 4, so this is the minimum a legitimate USP already satisfies.
 *
 * It is enforced because an in-bounds MISALIGNED USP makes every one of those stores
 * misaligned. On a core that traps them the nested trap re-enters the same prologue, rebuilds
 * one frame lower, faults again, and descends per iteration with no watchdog: no write lands
 * and the kernel live-locks on an unprivileged thread's say-so. */
#define KICKOS_RX_TRAP_SP_ALIGN 4

/* struct arch_context field offsets the guards read as plain displacements. switch.S .equ's
 * from these and arch_rxv3.cc static_asserts offsetof against them, so a field inserted ahead
 * of the bounds breaks the build. That assert is the whole check for this ISA: the RX reaches
 * a witness only on the bench, so a guard reading trace_tid as stack_hi has to be caught at
 * compile time. */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_RX_CTX_OFF_SP 0
#define KICKOS_RX_CTX_OFF_TRACE_TID 4
#define KICKOS_RX_CTX_OFF_STACK_LO 8
#define KICKOS_RX_CTX_OFF_STACK_HI 12
#else
#define KICKOS_RX_CTX_OFF_SP 0
#define KICKOS_RX_CTX_OFF_STACK_LO 4
#define KICKOS_RX_CTX_OFF_STACK_HI 8
#endif

#endif /* KICKOS_ARCH_RX_TRAP_STACK_H */
