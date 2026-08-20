/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extent trap_entry (switch.S) reserves at the BOTTOM of an interrupted U-mode
 * stack before it agrees to build a frame there. Plain #defines so switch.S and
 * arch_rv32imac.cc read ONE source of truth.
 *
 * WHY AN EXTENT AND NOT A POINTER TEST. The prologue validates the interrupted sp and
 * then builds the frame DOWNWARD from it, and the kernel's own C dispatch then runs on
 * that same stack below the frame (.Lecall calls kickos_ipc_fastpath; .Lecall_slow mrets
 * into svc_trampoline, which calls syscall_dispatch; every .Lintr arm calls its ISR).
 * All of it runs M-mode, which bypasses the unlocked PMP entries. A test that only asks
 * whether sp itself is inside [stack_lo, stack_hi) therefore lets a U-mode thread aim sp
 * one word above stack_lo and have the kernel write privileged, at an address the thread
 * chose, into whatever sits below: with no padding between thread stacks that is the
 * NEIGHBOUR's saved frame, whose mepc/mstatus the neighbour's mret then loads.
 * So the bound is on the room REMAINING below sp, and it must cover the frame PLUS the
 * deepest kernel descent that trap can reach.
 *
 * The two figures below are MEASURED. tests/static/check_trap_redzone.sh re-measures them:
 * it rebuilds the tree under -fcallgraph-info=su,da and fails when the worst-case depth from
 * the trap-path roots exceeds what they reserve.
 *
 * NOT covered by the two red zones: a trap taken while svc_trampoline is already running the
 * dispatch. It enters with mstatus.MPP=M, so no bound applies, and the sp it interrupts is
 * the CALLING THREAD'S, at whatever depth the dispatch has reached (dispatch runs on the
 * caller's continuation, arch.h arch_syscall contract). A thread that parks sp at exactly the
 * red-zone edge and then makes a deep syscall would have the next tick build 128 bytes, plus
 * that ISR's own descent, below stack_lo, privileged, in whatever lies beneath.
 * .Ltrap_from_m therefore keeps the trusted per-hart trap stack for every M-mode cause whose
 * frame is not a thread's saved context, and KICKOS_RV_TRAP_NESTED_DEPTH below sizes that
 * stack. The two causes it cannot move are msip and ecall-from-M, whose frames ARE the saved
 * context; what that leaves is stated at KICKOS_RV_TRAP_KERNEL_DEPTH_SYS.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* The save frame trap_entry builds below the interrupted sp: `addi sp, sp, -128` then 30
 * word stores at 0(sp)..124(sp). Must equal FRAME in switch.S and FRAME_WORDS*4 in
 * arch_rv32imac.cc, both of which static_assert against it. */
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
 * Two figures and not one: mcause separates the classes and is readable before the frame is
 * built, so each trap pays only its own class.
 *
 * WHAT THESE DELIBERATELY EXCLUDE, and the residual it leaves: the noreturn kpanic tail
 * (kpanic -> kputs -> kconsole_write_impl[176] -> console_emit -> arch_console_write),
 * which every KICKOS_ASSERT in the dispatch reaches. Including it takes _TRAP to 368 and
 * _SYS to 800, which puts the syscall red zone at 1056, ABOVE KICKOS_MIN_STACK_SIZE, so a
 * thread spawned at the floor could not make a syscall at all. So the residual is real and
 * it is this: if a kernel assertion fires while a thread is parked at the very bottom of its
 * red zone, the console writer descends up to 96 bytes (syscall, 1056 against the 960
 * reserved) or 112 bytes (trap, 496 against 384) below stack_lo, privileged. Reaching it
 * costs a kernel assertion failure and the system is terminating, but it is NOT covered.
 * check_trap_redzone.sh prints both figures.
 *
 * NOT excluded and NOT measured either, so it is stated here: .Lfault hands an ACCEPTED
 * U-mode fault frame to kickos_rv_fault_report on the thread's own stack, and that symbol is
 * not in the TRAP root set. Its deep chain (656, the figure that sizes the trap stack below)
 * is unreachable there, because kickos_fault_kill_thread answers yes for an in-bounds U-mode
 * frame and the reporter returns before the dump; what does run on the thread stack is the
 * kill path alone. That is a claim about C control flow, which no call graph carries.
 *
 * The winning TRAP chain runs through an INDIRECT call, the SchedPolicy hook table, which
 * no call graph resolves on its own: tests/static/trap_redzone_indirect.txt binds each such
 * site to the one slot its source line calls, and the gate refuses to answer at all while a
 * reachable site is unbound.
 *
 * Every figure ENFORCED here is its measurement rounded up to the next multiple of 64
 * (224 -> 256, 688 -> 704). The gate fails when a measurement EXCEEDS the figure, so the
 * slack cannot be spent silently. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 256
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 704

/* THE SYSCALL ZONE HOLDS TWO FRAMES, and the second is the msip frame the deferred switcher
 * builds. A blocking dispatch pends msip and the trap fires at whatever depth the dispatch
 * had reached; so does a tick's wake, whose pended msip fires on the mret back INTO the
 * dispatch, which puts it as deep as the dispatch ever goes. That frame is the outgoing
 * thread's saved CONTEXT (.Lswitch stores it as outgoing->sp), so it is the one M-mode frame
 * the trusted trap stack cannot take and the room for it has to be reserved here. Without
 * this term a thread that parks sp at the edge and calls the deepest syscall has 128 bytes
 * written below stack_lo, privileged, and then resumes through a frame whatever lies there
 * could have rewritten. 256 rather than 2 * KICKOS_RV_TRAP_FRAME because the gate scrapes
 * this macro as a plain integer; arch_rv32imac.cc asserts the two agree.
 *
 * The room a syscall really needs is FRAME + d + FRAME + s, for a dispatch depth d and the
 * switcher's own descent s below the frame it saved. The figure below is exact only while
 * d + s <= KICKOS_RV_TRAP_KERNEL_DEPTH_SYS, and what holds that is s == 0, which is what the
 * gate's SWITCH class re-measures. A switcher hook that gained a frame fails that class
 * rather than eating this margin in silence. */
#define KICKOS_RV_TRAP_FRAME_SYS 256

/* Bytes .Lswitch's own C descends BELOW the frame it saved, over the roots it calls:
 * kickos_arch_mpu_commit, plus kickos_trace_switch_done and kickos_bench_switch_done where
 * their options are on. MEASURED 0 on both boards the gate runs (kickos_arch_mpu_commit is a
 * 0-byte frame with no callee, and the two hooks are compiled out). */
#define KICKOS_RV_TRAP_SWITCH_DEPTH 0

/* What the guard enforces: room below sp, in bytes. Both stay under gas's 12-bit signed
 * addi/li range, so a figure that outgrew the encoding would fail to assemble. */
#define KICKOS_RV_TRAP_REDZONE (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_KERNEL_DEPTH)
#define KICKOS_RV_TRAP_REDZONE_SYS \
    (KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYS)

/* The trusted per-hart trap stack (g_rv_trap_stack, arch_rv32imac.cc). ONE frame, because
 * MIE is 0 from the vector to the mret and no interrupt can nest on a trap-stack frame; a
 * synchronous FAULT can, and it reuses this same frame slot, which is sound only because
 * that fault is a kernel bug: arch_fault_is_user_thread refuses an M-mode frame, so
 * kickos_rv_fault_report terminates and the frame it overwrote is never resumed.
 *
 * The depth is the same measurement, same instrument, over the roots switch.S reaches on the
 * nested path. The four .Lintr ISR arms are shallower (timer 224, ext 176, soft 160, extdev
 * 0), so .Lfault's reporter is what sizes this stack, and it runs here for every M-mode fault
 * as well as for the wild-sp refusal that already ran here.
 *
 * TWO BOARDS, AND THE FIGURE IS THE WORSE OF THEM, because the gate measures one board per
 * run and the console backend is per board:
 *   qemu-riscv        672  kickos_rv_fault_report[32] -> kickos_isr_fault[32]
 *                          -> kprintf[64] -> kvprintf_route[288] -> kconsole_write[0]
 *                          -> kconsole_write_impl[176] -> console_emit[48]
 *                          -> arch_console_write[32]
 *   esp32c6-wroom-st  768  the same chain, but its arch_console_write is a 0-byte thunk into
 *                          console_tx_write[80] -> drain_sync[32] -> wait_slot[16], the TX
 *                          ring the C6 drives instead of a polled register.
 *
 * The kpanic tail is COUNTED here and excluded from the two above: what forbids counting it
 * there is the spawn floor, and this is one static kernel array with no floor to clear. A
 * panic that overran this stack would corrupt the report it was writing.
 *
 * THIS CLASS IS THE ONE THAT WALKS THE CONSOLE, so a console change moves it. 832 is
 * deliberately 64 above the 768 measured: the enforced figure is what a FUTURE change is
 * measured against, and 64 bytes buys that margin in ONE shared kernel array at no
 * per-thread cost. Do NOT tighten it back to the measurement. */
#define KICKOS_RV_TRAP_NESTED_DEPTH 832
#define KICKOS_RV_TRAP_STACK_SIZE \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH)

/* Alignment the guard requires of an interrupted U-mode sp. The RISC-V psABI keeps sp
 * 16-byte aligned, arch_context_init aligns the stack top to 16 and FRAME is a multiple
 * of it, so every legitimate sp satisfies this and .Lfault already re-aligns to 16 to
 * make a call.
 *
 * It is enforced because an in-bounds MISALIGNED sp makes every store in the frame build
 * misaligned. On a core that traps those, the nested trap re-enters the prologue, rebuilds
 * at sp-128, faults again, and descends 128 bytes per iteration with no watchdog: no write
 * lands, and the kernel live-locks on an unprivileged thread's say-so. QEMU virt completes
 * misaligned stores and cannot witness it; the ESP32-C6 is the exposure. */
#define KICKOS_RV_TRAP_SP_ALIGN 16

/* Byte offset of the s2 slot from the frame base. Exported for the faultsurvive kwrite arm,
 * which aims a U-mode sp so that THIS slot's store lands on the kernel witness word: it needs
 * the offset and the frame size together. switch.S asserts its own F_S2 against this. */
#define KICKOS_RV_TRAP_F_S2 64

/* Byte offset of the slot carrying the sp .Lrestore leaves on. The 30 saved words reach
 * 116, so this sits in the 8 bytes the 16-byte-aligned frame size already had spare and the
 * frame did not grow. THREE places write it and nothing else may: the two trap_entry
 * builders and arch_context_init, which is the only other code that fabricates a frame. A
 * fabricated frame that leaves it zero resumes on a null sp at the first mret. */
#define KICKOS_RV_TRAP_F_SP 120

/* mcause for a machine software interrupt: bit 31 set, code 3. The one INTERRUPT cause whose
 * frame is a thread's saved context, so .Ltrap_from_m has to name it rather than treat every
 * interrupt alike. switch.S reads it as an li immediate. */
#define KICKOS_RV_MCAUSE_MSIP 0x80000003

/* struct arch_context field offsets the guard reads as plain words, and the frame slots
 * the switcher reads. ONE definition rather than a literal in switch.S and a mirror in
 * arch_rv32imac.cc: switch.S .equ's from these, arch_rv32imac.cc static_asserts offsetof
 * against them. Without that, a field inserted ahead of the bounds makes the guard read
 * trace_tid as stack_hi. */
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
