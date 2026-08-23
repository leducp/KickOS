/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the RXv3 trap sites (switch.S) reserve: two at the BOTTOM of a thread's own
 * stack, before they write through the live USP, and one at the top of that thread's
 * per-thread KERNEL block, which the syscall trap's generic arm transfers to.
 *
 * AN EXTENT AND NOT A POINTER TEST. Every store these sites make through a live USP runs
 * SUPERVISOR, and the RX MPU checks user mode only (RX72M UM sec.17.1.1), so supervisor
 * bypasses it. A test that only asked whether the USP lies inside [stack_lo, stack_hi) would
 * let a user thread park it one word above stack_lo and have the kernel write supervisor into
 * the NEIGHBOUR's saved frame, there being no padding between pool stacks: the rte resuming
 * the neighbour pops PC then PSW from it, so clearing PSW.PM there hands it supervisor.
 *
 * THE SYSCALL DISPATCH IS ON THE KERNEL BLOCK, svc_trampoline relocating R0 onto the calling
 * thread's block (ctx.kernel_sp) first, so it is a requirement on KICKOS_KERNEL_STACK_SIZE.
 * The eight-byte [userPC][userPSW] head the generic arm leaves on the thread's own stack is
 * not optional: RX has no way to leave user mode but RTE, RTE pops PC then PSW from R0, and
 * R0 is the USP while PSW.U is set, so the resume USP IS the head's address plus eight.
 *
 * Four classes, separated by WHICH STACK they land on:
 *   PENDSW    the deferred switcher's context save on the interrupted thread's own stack; it
 *             is that thread's saved context and ctx.sp names it, so it does not move.
 *   SYS       the generic arm's leavings on that same stack: the head above plus the switcher
 *             zone a SWINT can put below it. No C frame at all, so the class has no root.
 *   SYS_FAST  the fastpath arm's 236-byte save on the caller's own USP, that frame BEING the
 *             caller's saved context, plus the restore descent it pays on resume.
 *   SYSK      both C dispatches on the caller's kernel block.
 *
 * tests/static/check_trap_redzone.sh re-measures the descent figures below and fails when a
 * worst case exceeds what the class reserves. The measured half and the structural half stay
 * SEPARATE macros: folded into one, a growing dispatch could eat the structural allowance and
 * the gate would still report room to spare.
 *
 * RX INTERRUPTS are accepted on the ISP, acceptance clearing PSW.U whatever stack was
 * interrupted, so a device ISR's frame never lands on a thread-chosen stack. kickos_rx_pendsw
 * rebuilds a save on a USP by hand, which is why a switcher zone is the only exception term
 * any class below carries.
 */

#ifndef KICKOS_ARCH_RX_TRAP_STACK_H
#define KICKOS_ARCH_RX_TRAP_STACK_H

/* Bytes the two frame builders write below the USP they arrive with, counted off the EMITTED
 * code (rx-elf-objdump on a linked image, -misa=v3 -mdfpu):
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
 * A DFPU-less build writes 96 rather than 236, so these are the -mdfpu worst case. */
#define KICKOS_RX_TRAP_FRAME_PENDSW 236
#define KICKOS_RX_TRAP_FRAME_SYS_FAST 236

/* THE SYS CLASS, structural half: everything the generic arm can put below the USP the guard
 * validates, with no C frame among it. P0 is that USP.
 *
 *   -8     the [userPC][userPSW] head, stored and committed with mvtc before the trap rewrites
 *          the stacked PC and RTEs. svc_trampoline's own rte pops the pair back off it, so P0
 *          is what the thread resumes on.
 *   -300   ONE WHOLE PENDSW ZONE, at KICKOS_RX_TRAP_REDZONE_PENDSW. The RTE restores the
 *          caller's PSW.I, so a timer can be accepted from the head's address onwards and a
 *          SWINT pended over it rebuilds its 236-byte save on this USP plus the 64-byte
 *          restore descent. Charged once, covering both sides of the write of R0.
 *
 * It CANNOT REPEAT: the switcher runs with PSW.I clear throughout and switches away, so at
 * most one such save is ever stacked on a thread's USP. */
#define KICKOS_RX_TRAP_NEST_SYS 308

/* THE SYSK CLASS, structural half: what stands between the top of a kernel block (T =
 * ctx.kernel_sp) and the first byte syscall_dispatch may use, plus what a preemption puts
 * below the deepest byte it uses.
 *
 *   -8     the continuation header svc_trampoline lays at T: at [T-8] a3, the fifth argument
 *          the psABI passes on the stack, and at [T-4] the USP the thread resumes on. That
 *          USP lives HERE because a blocking dispatch has the switcher freeze its
 *          continuation BELOW this header, so the header survives the block for free. No pad
 *          term: RX stack alignment is 4 bytes and every store here is a word store.
 *   -300   ONE WHOLE PENDSW ZONE again, below the deepest byte of the dispatch.
 *
 * 308 is the same figure the SYS class carries and for a different reason, so the two stay
 * separate macros. */
#define KICKOS_RX_TRAP_NEST_SYSK 308

/* Worst-case bytes the kernel's C dispatch descends BELOW each of those, per class, measured
 * on rx72m-st (MinSizeRel, -fcallgraph-info=su,da) as the longest weighted path over the
 * merged call graph from the roots switch.S branches to. On rx-elf the compiler's reported
 * frame ALREADY includes the incoming return-address slot, so the chains below are not
 * missing a per-level 4.
 *
 *   _PENDSW      32  kickos_arch_mpu_commit[28] -> arch_irq_restore[4], the only C the
 *                    restore epilogue runs below an incoming frame. The INCOMING thread's
 *                    stack pays it, checked against this same figure when it was switched out.
 *   _SYS          0  svc_trampoline moves R0 to ctx.kernel_sp before it calls anything.
 *   _SYS_FAST    32  the same epilogue and so the same chain as _PENDSW. It stays a macro of
 *                    its own because it is a distinct SITE with its own guard.
 *   _SYSK       788  the deeper of the two roots on the kernel block, with the panic tail
 *                    COUNTED, trap_redzone_roots.txt declaring SYSK stack=kernel and a
 *                    stack=kernel class being measured with no exclusion. kickos_ipc_fastpath
 *                    measures 456 there and is dominated; syscall_dispatch sets it:
 *                    syscall_dispatch[36] -> syscall_body[108] -> thread_spawn[244]
 *                    -> cap_install_defaults[4] -> cap_seat_stdout[40] -> obj_ref_inc[20]
 *                    -> ref_counters[28] -> kpanic[8] -> kputs[8] -> kconsole_write[4]
 *                    -> kconsole_write_impl[152] -> console_emit[32]
 *                    -> arch_console_write[4] -> console_tx_write[48] -> drain_sync[28]
 *                    -> wait_slot[20] -> the indirect call at console_tx.cc:69:37
 *                    -> rx_tx_slot_free[4].
 *
 * ENFORCED figures are those measurements rounded up, 32 -> 64. _SYSK is NOT rounded, a kernel
 * block being sized to it directly, so a byte of slack there costs KICKOS_THREAD_SLOTS bytes.
 *
 * rx72m is the only chip that selects ARCH_RXV3 and trap_redzone_roots.txt declares three of
 * its presets, so a figure here is the worse of three readings. No telemetry build is among
 * them: _PENDSW's optional trace and bench roots are ABSENT from this graph and contribute 0
 * to the 32 above. A re-measurement MUST carry this board's -misa=v3 -mdfpu baseline; at the
 * compiler's default -misa=v1 with no DFPU every figure comes out smaller than the truth. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW 64
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYS 0
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYS_FAST 64
/* 792 and not the 788 the chain above sums to: kos_thread_spawn validates a CALLER-SUPPLIED
 * stack against the TLS stride, and that inlined check grows thread_spawn's own frame, the
 * deepest on the chain, to 248. The zone becomes 1100 against a 1104-byte block. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYSK 792

/* THE EXIT CLASS, structural half: what a preemption puts below the deepest byte the DEATH
 * PATH uses. One whole PENDSW zone, 236 of save plus the 64 restore descent below it, which
 * is KICKOS_RX_TRAP_REDZONE_PENDSW read from the other end and what switch.S computes as
 * BLOCK_USP_MAX_DROP = SIZE - REDZONE_PENDSW - 4. A FRAME TERM and not 0: the stub runs at
 * PSW.I = 1 with IPL = 0, which is what lets its exit_current reschedule.
 *
 * PLUS THE EIGHT BYTES THE ENTRY SKIPS. arch_fault_redirect_to_exit seats the USP at
 * kernel_sp - 8, because kickos_rx_pendsw's block leg tests the distance with bleu and reads
 * a USP exactly at the top as zero distance, refusing it. Those 8 bytes sit above every byte
 * the descent uses, so the block holds them too: 8 + 300 is 308.
 *
 * A PLAIN INTEGER and not the expression, because check_trap_redzone.sh scrapes this macro as
 * an immediate and REFUSES a figure it cannot find as one. arch_rxv3.cc asserts the
 * composition instead. */
#define KICKOS_RX_TRAP_NEST_EXIT 308

/* The measured descent of the two stubs a dying thread runs PRIVILEGED on its own KERNEL
 * BLOCK, kickos_fault_stack_top answering with ctx.kernel_sp: kickos_thread_fault_exit and
 * kickos_thread_slay_exit. 616, the fault stub the deeper through kprintf_fault's console
 * chain, identical on all three registered presets.
 *
 * IT NEVER BINDS, and this is the arch with least room for that to change: 300 + 616 = 916
 * against 1100 usable, where SYSK asks 1096, so EXITK would have to grow 180 bytes before it
 * displaced SYSK and forced a KICKOS_KERNEL_STACK_SIZE raise. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_EXITK 616

/* kickos_thread_return ALONE: an ordinary privileged thread's entry returning, with no fault
 * and no redirect, so it runs at whatever depth the entry returned from on the thread's own
 * stack. 476, identical on all three registered presets. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_RET 476

/* What each guard enforces: room below the USP, in bytes.
 *
 * The syscall figure is ONE constant covering both arms: the guard sits at the top of
 * kickos_rx_syscall_trap, above the `cmp #56, r1` that chooses the arm, so it runs before the
 * arm is known, and arch_rxv3.cc static_asserts that it dominates both arms in both
 * directions. THE GENERIC ARM SETS IT, at 308; the fastpath's 236 + 64 is smaller because it
 * carries no switcher zone of its own, checking PSW.I, refusing an entry still carrying it,
 * and setting it nowhere. The 64 is its restore descent, paid below its own save on resume. */
#define KICKOS_RX_TRAP_REDZONE_PENDSW \
    (KICKOS_RX_TRAP_FRAME_PENDSW + KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW)
#define KICKOS_RX_TRAP_REDZONE_SYS 308

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE and not a bound
 * anything refuses at run time, every byte of it being written by privileged code through a
 * pointer the kernel seated. arch_rxv3.cc static_asserts the block against it, and
 * check_trap_redzone.sh compares the same pair against the block the board configured. */
#define KICKOS_RX_TRAP_NEED_SYSK \
    (KICKOS_RX_TRAP_NEST_SYSK + KICKOS_RX_TRAP_KERNEL_DEPTH_SYSK)

/* The [userPC][userPSW] head kickos_rx_syscall_trap commits on the calling thread's stack
 * before its RTE, and which svc_trampoline's R0 sits below. Two words, because RTE pops PC
 * then PSW and nothing else. switch.S consumes this rather than repeating the literal, so the
 * head and the displacements that step over it cannot drift apart.
 *
 * NOT KICKOS_RX_TRAP_HEAD_ABOVE_SYS, which is 8 for a different reason: that one is how far
 * ABOVE the entry USP the trap path reads. The kernel block's continuation header is a THIRD
 * 8 with a third derivation. */
#define KICKOS_RX_TRAP_HEAD_SYS 8

/* Bytes the trap path reads ABOVE the USP the syscall guard validates, which the bound on that
 * USP has to reserve. svc_trampoline reads a3 with `mov.l 12[r0], r5`, and its R0 is the entry
 * USP minus the eight-byte head the trap committed, so the read lands at entry+4 and ends at
 * entry+8: the psABI fifth-argument slot arch_syscall left at [R0+4]. A bound that merely kept
 * the USP AT OR BELOW stack_hi would let that read run past the top of the stack. It refuses
 * no legitimate USP: a thread in arch_syscall has already pushed the return address its bsr
 * left and the a3 slot above it. */
#define KICKOS_RX_TRAP_HEAD_ABOVE_SYS 8

/* Alignment the guards require of a live USP. Every store in both frame builds is a word store
 * (mov.l, push.l, pushm, and the DPFPU bank on top of them), the head the generic arm stores
 * is two more, and arch_context_init aligns the stack top to 4. It is enforced because an
 * in-bounds MISALIGNED USP makes every one of those stores misaligned: on a core that traps
 * them the nested trap re-enters the same prologue, rebuilds one frame lower, faults again,
 * and descends per iteration with no watchdog, so no write lands and the kernel live-locks on
 * an unprivileged thread's say-so. */
#define KICKOS_RX_TRAP_SP_ALIGN 4

/* struct arch_context field offsets the trap sites read as plain displacements. switch.S
 * .equ's from these and arch_rxv3.cc static_asserts offsetof against them. That assert is the
 * whole check for this ISA: the RX reaches a witness only on the bench, so a guard reading
 * trace_tid as stack_hi has to be caught at compile time. */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_RX_CTX_OFF_SP 0
#define KICKOS_RX_CTX_OFF_TRACE_TID 4
#define KICKOS_RX_CTX_OFF_STACK_LO 8
#define KICKOS_RX_CTX_OFF_STACK_HI 12
#define KICKOS_RX_CTX_OFF_KERNEL_SP 16
#else
#define KICKOS_RX_CTX_OFF_SP 0
#define KICKOS_RX_CTX_OFF_STACK_LO 4
#define KICKOS_RX_CTX_OFF_STACK_HI 8
#define KICKOS_RX_CTX_OFF_KERNEL_SP 12
#endif

#endif /* KICKOS_ARCH_RX_TRAP_STACK_H */
