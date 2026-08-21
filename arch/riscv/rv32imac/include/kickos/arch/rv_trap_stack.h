/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent trap_entry (switch.S) reserves at the BOTTOM of an interrupted U-mode stack
 * before it agrees to build a frame there.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. The prologue validates the interrupted sp, builds
 * the frame DOWNWARD from it, and the kernel's own C dispatch then runs on that same stack
 * below the frame. All of it runs M-mode, which bypasses the unlocked PMP entries. A test
 * that only asked whether sp itself lies inside [stack_lo, stack_hi) would let a U-mode
 * thread aim sp one word above stack_lo and have the kernel write privileged, at an address
 * the thread chose, into whatever sits below: with no padding between thread stacks that is
 * the NEIGHBOUR's saved frame, whose mepc/mstatus the neighbour's mret then loads. So the
 * bound is on the room REMAINING below sp, and it covers the frame PLUS the deepest kernel
 * descent that trap can reach.
 *
 * tests/static/check_trap_redzone.sh re-measures the depth figures below under
 * -fcallgraph-info=su,da and fails when a worst-case path exceeds what they reserve.
 *
 * A trap taken while svc_trampoline is running the dispatch enters with mstatus.MPP=M, so no
 * bound applies, and the sp it interrupts is the CALLING THREAD'S, at whatever depth the
 * dispatch has reached (dispatch runs on the caller's continuation, arch.h arch_syscall
 * contract). .Ltrap_from_m therefore moves every M-mode cause onto the trusted per-hart trap
 * stack, which KICKOS_RV_TRAP_NESTED_DEPTH sizes. The two causes it keeps on the interrupted
 * stack are msip and ecall-from-M, whose frames ARE a thread's saved context;
 * KICKOS_RV_TRAP_KERNEL_DEPTH_SYS reserves for those.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* The save frame trap_entry builds below the interrupted sp: `addi sp, sp, -128`, then 30
 * word stores spanning 0(sp)..116(sp) plus the F_SP slot at 120. Equals FRAME in switch.S
 * and FRAME_WORDS*4 in arch_rv32imac.cc, both of which assert against it. */
#define KICKOS_RV_TRAP_FRAME 128

/* Worst-case bytes the kernel's C dispatch descends BELOW the frame, per trap class.
 * Measured on qemu-riscv (MinSizeRel, -fcallgraph-info=su,da) as the longest weighted
 * path over the merged call graph, rooted at the symbols switch.S actually branches to:
 *
 *   _TRAP  224  kickos_isr_timer[0] -> ktime_on_timer[64] -> endpoint_wait_abort[32]
 *               -> sched::wake[16] -> resched_after_wake[32] -> reschedule[32]
 *               -> the indirect call at sched.cc:43:42 -> policy_on_switch_in[16]
 *               -> arm_slice[32]
 *   _SYS   688  syscall_dispatch[32] -> syscall_body[128] -> thread_spawn[272]
 *               -> thread_create[96] -> task_for[16] -> domain_for[32]
 *               -> grant_region_admissible[32] -> grant_hits_reserved[80]
 *
 * Two classes and not one because mcause is readable before the frame is built, so each
 * trap pays only its own class.
 *
 * BOTH FIGURES EXCLUDE the noreturn kpanic tail (kpanic -> kputs -> kconsole_write_impl[176]
 * -> console_emit -> arch_console_write), which every KICKOS_ASSERT in the dispatch reaches.
 * Including it takes _TRAP to 368 and _SYS to 800, which puts the syscall red zone at 1056,
 * ABOVE KICKOS_MIN_STACK_SIZE, so a thread spawned at the floor could not make a syscall at
 * all. The residual that leaves: a kernel assertion firing while a thread is parked at the
 * very bottom of its red zone has the console writer descend up to 96 bytes (syscall, 1056
 * against the 960 reserved) or 112 bytes (trap, 496 against 384) below stack_lo, privileged,
 * with the system already terminating. check_trap_redzone.sh prints both figures.
 *
 * .Lfault runs kickos_rv_fault_report on the thread's own stack for an ACCEPTED U-mode fault
 * frame, and only the kill path runs there: kickos_fault_kill_thread answers yes for an
 * in-bounds U-mode frame, so the reporter returns before the 656-byte dump chain (the figure
 * that sizes the trap stack below). That is a claim about C control flow, which no call graph
 * carries, so it is stated here rather than measured.
 *
 * The winning TRAP chain runs through an INDIRECT call, the SchedPolicy hook table:
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer at all while a reachable site is unbound.
 *
 * Every figure ENFORCED here is its measurement rounded up to the next multiple of 64
 * (224 -> 256, 688 -> 704). The gate fails when a measurement EXCEEDS the figure, so the
 * slack cannot be spent silently. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 256
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 704

/* THE SYSCALL ZONE HOLDS TWO FRAMES, the second being the msip frame the deferred switcher
 * builds. A blocking dispatch pends msip and the trap fires at whatever depth the dispatch
 * had reached; a tick's wake pends msip too, and that one fires on the mret back INTO the
 * dispatch, as deep as the dispatch ever goes. That frame is the outgoing thread's saved
 * CONTEXT (.Lswitch stores it as outgoing->sp), so it is the one M-mode frame that stays on
 * the thread's stack, and the room for it is reserved here.
 *
 * A plain 256 and not 2 * KICKOS_RV_TRAP_FRAME: the gate scrapes this macro as an integer
 * immediate. arch_rv32imac.cc asserts the two agree.
 *
 * The room a syscall really needs is FRAME + d + FRAME + s, for a dispatch depth d and the
 * switcher's own descent s below the frame it saved. The figure below is exact while
 * d + s <= KICKOS_RV_TRAP_KERNEL_DEPTH_SYS, and what holds that is s == 0, which the gate's
 * SWITCH class re-measures: a switcher hook that gained a frame fails that class rather than
 * eating this margin in silence. */
#define KICKOS_RV_TRAP_FRAME_SYS 256

/* Bytes .Lswitch's own C descends BELOW the frame it saved, over the roots it calls:
 * kickos_arch_mpu_commit, plus kickos_trace_switch_done and kickos_bench_switch_done where
 * their options are on. MEASURED 0 on both boards the gate runs: kickos_arch_mpu_commit is a
 * 0-byte frame with no callee, and the two hooks are compiled out. */
#define KICKOS_RV_TRAP_SWITCH_DEPTH 0

/* What the guard enforces: room below sp, in bytes. switch.S materialises each with `li`
 * into a register and compares with `bltu`, so no immediate field bounds either figure. */
#define KICKOS_RV_TRAP_REDZONE (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_KERNEL_DEPTH)
#define KICKOS_RV_TRAP_REDZONE_SYS \
    (KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYS)

/* The trusted per-hart trap stack (g_rv_trap_stack, arch_rv32imac.cc). ONE frame: MIE is 0
 * from the vector to the mret, so no interrupt nests on a trap-stack frame. A synchronous
 * FAULT does nest, and it reuses this same frame slot, which is sound because such a fault is
 * a kernel bug: arch_fault_is_user_thread refuses an M-mode frame, so kickos_rv_fault_report
 * terminates and the frame it overwrote is never resumed.
 *
 * Same measurement and instrument as above, over the roots switch.S reaches on the nested
 * path. The four .Lintr ISR arms are shallower (timer 224, ext 176, soft 160, extdev 0), so
 * .Lfault's reporter is what sizes this stack, and it runs here for every M-mode fault as
 * well as for the wild-sp refusal that already ran here.
 *
 * TWO BOARDS, AND THE FIGURE IS THE WORSE OF THEM, since the gate measures one board per run
 * and the console backend is per board:
 *   qemu-riscv        672  kickos_rv_fault_report[32] -> kickos_isr_fault[32]
 *                          -> kprintf[64] -> kvprintf_route[288] -> kconsole_write[0]
 *                          -> kconsole_write_impl[176] -> console_emit[48]
 *                          -> arch_console_write[32]
 *   esp32c6-wroom-st  768  the same chain, but its arch_console_write is a 0-byte thunk into
 *                          console_tx_write[80] -> drain_sync[32] -> wait_slot[16], the TX
 *                          ring the C6 drives instead of a polled register.
 *
 * The kpanic tail IS counted here, unlike the two figures above: what forbids counting it
 * there is the spawn floor, and this is one static kernel array with no floor to clear.
 *
 * THIS CLASS IS THE ONE THAT WALKS THE CONSOLE, so a console change moves it. 832 is
 * deliberately 64 above the 768 measured: the enforced figure is what a FUTURE change is
 * measured against, and the margin costs one shared kernel array rather than per-thread
 * bytes. Do NOT tighten it back to the measurement. */
#define KICKOS_RV_TRAP_NESTED_DEPTH 832
#define KICKOS_RV_TRAP_STACK_SIZE \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH)

/* Alignment the guard requires of an interrupted U-mode sp. The RISC-V psABI keeps sp
 * 16-byte aligned, arch_context_init aligns the stack top to 16 and FRAME is a multiple of
 * it, so every legitimate sp satisfies this, and .Lfault re-aligns to 16 to make a call.
 *
 * It is enforced because an in-bounds MISALIGNED sp makes every store in the frame build
 * misaligned. On a core that traps those, the nested trap re-enters the prologue, rebuilds at
 * sp-128, faults again, and descends 128 bytes per iteration with no watchdog: no write
 * lands, and the kernel live-locks on an unprivileged thread's say-so. The ESP32-C6 traps
 * misaligned stores; QEMU virt completes them. */
#define KICKOS_RV_TRAP_SP_ALIGN 16

/* Byte offset of the s2 slot from the frame base. Exported for the faultsurvive kwrite arm,
 * which aims a U-mode sp so that THIS slot's store lands on the kernel witness word: it needs
 * the offset and the frame size together. switch.S asserts its own F_S2 against this. */
#define KICKOS_RV_TRAP_F_S2 64

/* Byte offset of the slot carrying the sp .Lrestore leaves on. The 30 saved words reach 116,
 * so this sits in the 8 bytes the 16-byte-aligned frame size already had spare and the frame
 * did not grow. THREE places write it and nothing else may: the two trap_entry builders and
 * arch_context_init, the only other code that fabricates a frame. A fabricated frame that
 * leaves it zero resumes on a null sp at the first mret. */
#define KICKOS_RV_TRAP_F_SP 120

/* mcause for a machine software interrupt: bit 31 set, code 3. The one INTERRUPT cause whose
 * frame is a thread's saved context, so .Ltrap_from_m names it rather than treating every
 * interrupt alike. switch.S reads it as an li immediate. */
#define KICKOS_RV_MCAUSE_MSIP 0x80000003

/* struct arch_context field offsets the guard reads as plain words, and the frame slots the
 * switcher reads. switch.S .equ's from these and arch_rv32imac.cc static_asserts offsetof
 * against them, so a field inserted ahead of the bounds breaks the build rather than leaving
 * the guard to read trace_tid as stack_hi. */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_RV_CTX_OFF_SP 0
#define KICKOS_RV_CTX_OFF_TRACE_TID 4
#define KICKOS_RV_CTX_OFF_STACK_LO 8
#define KICKOS_RV_CTX_OFF_STACK_HI 12
#else
#define KICKOS_RV_CTX_OFF_SP 0
#define KICKOS_RV_CTX_OFF_STACK_LO 4
#define KICKOS_RV_CTX_OFF_STACK_HI 8
#endif

#endif /* KICKOS_ARCH_RV_TRAP_STACK_H */
