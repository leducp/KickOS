/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The geometry trap_entry (switch.S) requires of the stacks it builds frames on: the
 * interrupted thread's own KERNEL stack for a U-mode entry and for the two M-mode causes
 * that keep the interrupted sp, and the trusted per-hart trap stack for every other M-mode
 * trap.
 *
 * NO FIGURE HERE RESERVES AN UNPRIVILEGED THREAD'S STACK. A U-mode entry loads ctx.kernel_sp
 * and builds the frame there, so the only thing the entry does with the sp a U-mode thread
 * chose is remember it in F_SP and test it: 16-byte aligned and inside
 * [stack_lo, stack_hi], because .Lrestore loads that word back and mret resumes U-mode on
 * it. Nothing privileged is stored through it, and no C runs below it. So the TRAP and SYS
 * figures below are a requirement on KICKOS_KERNEL_STACK_SIZE, which is what
 * check_trap_redzone.sh measures them against.
 *
 * THE ONE CASE THEY STILL PRICE A THREAD STACK is a PRIVILEGED thread's own syscall: it
 * traps with mstatus.MPP=M, so .Ltrap_from_m_ctx keeps the frame on the sp it interrupted,
 * which is that thread's own stack, and svc_trampoline runs the same dispatch there. Nothing
 * would be isolated by moving it, the caller being M-mode already, but the ROOM is the same
 * FRAME_SYS plus dispatch and that stack has to hold it. The SYSPRIV class measures exactly
 * that, against KICKOS_RV_TRAP_KERNEL_DEPTH_SYS_NO_PANIC.
 *
 * SO THE SPAWN FLOOR ON THIS ARCH IS NOW SET BY PRIVILEGED THREADS, and that is the
 * interesting result of the transfer. Two terms, one per privilege, and only the second binds:
 *
 *   UNPRIVILEGED, 720 bytes. Its dispatch is gone from its stack entirely. What privileged
 *   code still runs there is the death path: an accepted fault redirects to
 *   kickos_thread_fault_exit with sp moved to the TOP of that stack (kickos_fault_stack_top),
 *   and it descends kickos_thread_fault_exit[16] -> kprintf_fault[64] -> kvprintf_route[288]
 *   -> kconsole_write[0] -> kconsole_write_impl[176] -> console_emit[48]
 *   -> arch_console_write[0] -> console_tx_write[80] -> drain_sync[32] -> wait_slot[16] on
 *   esp32c6-wroom-st, 624 on qemu-riscv, over the same three exit roots (fault exit, slay
 *   exit, thread return) and the same instrument as the figures below. There is no frame term:
 *   the stub starts from the top.
 *
 *   PRIVILEGED, 960 bytes: KICKOS_RV_TRAP_FRAME_SYS 256 plus
 *   KICKOS_RV_TRAP_KERNEL_DEPTH_SYS_NO_PANIC 704, its ecall frame and dispatch staying where
 *   they always were. This is the term KICKOS_MIN_STACK_SIZE has to clear, and the FAILURE it
 *   prevents changed with the transfer: a red zone used to make the entry REFUSE a floor-sized
 *   thread's syscall, and there is no bound on an M-mode sp to refuse anything with, so what a
 *   floor below 960 buys now is a privileged thread overflowing its own stack mid-dispatch.
 *
 * tests/static/check_trap_redzone.sh re-measures the depth figures below under
 * -fcallgraph-info=su,da and fails when a worst-case path exceeds what they enforce, when a
 * kernel-stack class's frame plus depth exceeds the block, or when a thread-stack class's
 * frame plus depth exceeds KICKOS_MIN_STACK_SIZE.
 *
 * A trap taken while svc_trampoline is running the dispatch enters with mstatus.MPP=M, so no
 * sp test applies, and the sp it interrupts is the calling thread's KERNEL stack, at
 * whatever depth the dispatch has reached (dispatch runs on the caller's continuation,
 * arch.h arch_syscall contract). .Ltrap_from_m moves every M-mode cause onto the trusted
 * per-hart trap stack, which KICKOS_RV_TRAP_NESTED_DEPTH sizes. The two causes it keeps on
 * the interrupted sp are msip and ecall-from-M, whose frames ARE a thread's saved context;
 * they land on that thread's kernel stack and KICKOS_RV_TRAP_FRAME_SYS reserves for them.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* The save frame trap_entry builds below the stack top it picked: `addi sp, sp, -128`, then
 * 30 word stores spanning 0(sp)..116(sp) plus the F_SP slot at 120. Equals FRAME in switch.S
 * and FRAME_WORDS*4 in arch_rv32imac.cc, both of which assert against it. */
#define KICKOS_RV_TRAP_FRAME 128

/* Worst-case bytes the kernel's C dispatch descends BELOW the frame, per trap class.
 * Measured (MinSizeRel, -fcallgraph-info=su,da) as the longest weighted path over the merged
 * call graph, rooted at the symbols switch.S actually branches to.
 *
 * BOTH FIGURES COUNT THE NORETURN KPANIC TAIL, which every KICKOS_ASSERT in the dispatch
 * reaches. What used to forbid counting it was the spawn floor a THREAD stack has to clear;
 * these are one kernel array per thread slot with no floor to clear, exactly as
 * KICKOS_RV_TRAP_NESTED_DEPTH has always been. So the tail is IN the winning chain rather
 * than under it, and the same instrument with the tail dropped gives 224 and 688, which is
 * what these figures were while the entry still built on the interrupted thread stack.
 *
 * COUNTING THE TAIL MAKES THESE CONSOLE-SHAPED, so the figure is the WORSE OF THE TWO BOARDS
 * the gate runs, as KICKOS_RV_TRAP_NESTED_DEPTH already was. esp32c6-wroom-st is the worse
 * one: its arch_console_write is a 0-byte thunk into the TX ring the C6 drives instead of a
 * polled register, which is 96 bytes deeper than the qemu-riscv leaf.
 *
 *   _TRAP  480  kickos_isr_timer[0] -> ktime_on_timer[64] -> endpoint_wait_abort[32]
 *               -> kpanic[16] -> kputs[16] -> kconsole_write[0] -> kconsole_write_impl[176]
 *               -> console_emit[48] -> arch_console_write[0] -> console_tx_write[80]
 *               -> drain_sync[32] -> wait_slot[16]        (qemu-riscv: 384)
 *   _SYS   912  syscall_dispatch[32] -> syscall_body[128] -> thread_spawn[272]
 *               -> thread_create[96] -> the same tail from kpanic down (qemu-riscv: 816)
 *
 * Two classes and not one because mcause is readable before the frame is built, so each
 * trap pays only its own class.
 *
 * EACH FIGURE IS ITS MEASUREMENT, not the measurement rounded up to the next multiple of 64
 * the thread-stack red zones carried. The SYS requirement on the block is
 * KICKOS_RV_TRAP_FRAME_SYS + this = 1168, and the LOWEST word of a block is its overflow
 * canary (kernel/thread/thread.cc), so KICKOS_KERNEL_STACK_SIZE is 1184 on this arch: 12
 * bytes of slack above the canary word. Rounding 912 up to 960 would spend 48 bytes per
 * thread slot to hide the next measurement instead of failing on it, and the gate fails when
 * a measurement EXCEEDS the figure.
 *
 * The TRAP chain WITHOUT the tail runs through an INDIRECT call, the SchedPolicy hook table;
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot its source line
 * calls, and the gate refuses to answer at all while a reachable site is unbound. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 480
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 912

/* THE SAME SYSCALL DISPATCH WITH THE NORETURN KPANIC TAIL EXCLUDED, which is the figure the
 * one remaining THREAD-stack class is measured against. A PRIVILEGED thread's ecall arrives
 * with mstatus.MPP=M, so .Ltrap_from_m_ctx keeps its frame on the sp it interrupted, that
 * thread's own stack, and svc_trampoline runs this same dispatch there: the transfer moves
 * U-mode callers off their stacks and leaves privileged ones exactly where they were.
 *
 * THE TAIL IS DROPPED HERE FOR THE ORIGINAL REASON, which the kernel-stack figures no longer
 * have: a thread stack has a SPAWN FLOOR to clear. Counting it would put this requirement at
 * 256 + 912 = 1168, above every rv32imac KICKOS_MIN_STACK_SIZE, so a privileged thread
 * spawned at the floor could not make a syscall at all.
 *   688  syscall_dispatch[32] -> syscall_body[128] -> thread_spawn[272]
 *        -> thread_create[96] -> task_for[16] -> domain_for[32]
 *        -> grant_region_admissible[32] -> grant_hits_reserved[80]
 * Both boards measure the same 688: with the console tail gone the chain no longer walks a
 * per-board backend. Rounded up to the next multiple of 64, the convention a thread-stack red
 * zone carries, so the slack cannot be spent silently.
 *
 * THE RESIDUAL, unchanged in shape from when this was the whole story: a kernel assertion
 * firing while a privileged thread is parked at the very bottom of its floor has the console
 * writer descend up to 208 bytes (1168 against the 960 reserved) below its stack_lo,
 * privileged, with the system already terminating. The gate prints the without-exclusions
 * figure so it cannot go quiet. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS_NO_PANIC 704

/* THE SYSCALL REQUIREMENT HOLDS TWO FRAMES, the second being the msip frame the deferred
 * switcher builds. A blocking dispatch pends msip and the trap fires at whatever depth the
 * dispatch had reached; a tick's wake pends msip too, and that one fires on the mret back
 * INTO the dispatch, as deep as the dispatch ever goes. That frame is the outgoing thread's
 * saved CONTEXT (.Lswitch stores it as outgoing->sp), so .Ltrap_from_m_ctx keeps it on the
 * interrupted sp, which during a dispatch is the kernel stack the ecall frame is already on.
 * The room for it is reserved here.
 *
 * A plain 256 and not 2 * KICKOS_RV_TRAP_FRAME: the gate scrapes this macro as an integer
 * immediate. arch_rv32imac.cc asserts the two agree.
 *
 * So the kernel-stack requirement is FRAME + d + FRAME + s, for a dispatch depth d and the
 * switcher's own descent s below the frame it saved. The figure below is exact while
 * d + s <= KICKOS_RV_TRAP_KERNEL_DEPTH_SYS, and what holds that is s == 0, which the gate's
 * SWITCH class re-measures: a switcher hook that gained a frame fails that class rather than
 * eating this margin in silence. */
#define KICKOS_RV_TRAP_FRAME_SYS 256

/* Bytes .Lswitch's own C descends BELOW the frame it saved, over the roots it calls:
 * kickos_arch_mpu_commit, plus kickos_trace_switch_done and kickos_bench_switch_done where
 * their options are on. MEASURED 0 on both boards the gate runs: kickos_arch_mpu_commit is a
 * 0-byte frame with no callee, and the two hooks are compiled out.
 *
 * ZERO IS THE PART THAT MATTERS, and not because of the syscall requirement above.
 * .Lswitch and arch_start call that hook with sp = incoming->sp, and for a thread that has
 * never trapped that sp is still the frame arch_context_init fabricated on its USER stack.
 * The exit condition of the whole entry conversion, no privileged C frame on user memory,
 * holds only because the hook is frameless. That is why this class survives the red zones. */
#define KICKOS_RV_TRAP_SWITCH_DEPTH 0

/* The trusted per-hart trap stack (g_rv_trap_stack, arch_rv32imac.cc). ONE frame: MIE is 0
 * from the vector to the mret, so no interrupt nests on a trap-stack frame. A synchronous
 * FAULT does nest, and it reuses this same frame slot, which is sound because such a fault is
 * a kernel bug: arch_fault_is_user_thread refuses a frame that is not on the running thread's
 * kernel stack, so kickos_rv_fault_report terminates and the frame it overwrote is never
 * resumed.
 *
 * Same measurement and instrument as above, over the roots switch.S reaches on the nested
 * path. The four .Lintr ISR arms are shallower: the deepest of them is the timer, and that
 * arm IS what KICKOS_RV_TRAP_KERNEL_DEPTH measures, 480 with the same tail counted. So
 * .Lfault's reporter is what sizes this stack, and it runs here for every M-mode fault as
 * well as for the refused-sp path that already ran here.
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
 * THIS FIGURE IS ENFORCED TWICE, over the same reporter chain on two different stacks. An
 * ACCEPTED U-mode fault runs .Lfault with sp on the frame the entry built, which is now the
 * kernel stack, so the dump lands in the block: the gate's FAULT class charges it
 * KICKOS_RV_TRAP_FRAME plus this depth, 960 of the block, and the block already holds the
 * 1168 the syscall requirement asks. What used to stand in for that was a claim about C
 * control flow, that only the early-returning kill path ran on a thread's own stack; the
 * transfer replaces the claim with a class the gate measures.
 *
 * THIS CLASS IS THE ONE THAT WALKS THE CONSOLE, so a console change moves it. 832 is
 * deliberately 64 above the 768 measured: the enforced figure is what a FUTURE change is
 * measured against, and the margin costs one shared kernel array rather than per-thread
 * bytes. Do NOT tighten it back to the measurement. */
#define KICKOS_RV_TRAP_NESTED_DEPTH 832
#define KICKOS_RV_TRAP_STACK_SIZE \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH)

/* Alignment the entry requires of an interrupted U-mode sp. The RISC-V psABI keeps sp
 * 16-byte aligned, arch_context_init aligns the stack top to 16 and FRAME is a multiple of
 * it, so every legitimate sp satisfies this, and .Lfault re-aligns to 16 to make a call.
 *
 * It is enforced because .Lrestore hands that word back to mret: a U-mode thread resumed on
 * a misaligned sp takes its next stack access as a misaligned trap, on a core that traps
 * those, and re-enters the entry with the same sp for as long as it is scheduled. The
 * ESP32-C6 traps misaligned stores; QEMU virt completes them. The same figure divides
 * KICKOS_KERNEL_STACK_SIZE, so every slot's top is a legal frame base. */
#define KICKOS_RV_TRAP_SP_ALIGN 16

/* Byte offset of the s2 slot from the frame base. Exported for the faultsurvive kwrite arm,
 * which aims a U-mode sp so that THIS slot's store would land on the kernel witness word: it
 * needs the offset and the frame size together. The entry refuses that sp on the extent test
 * and would not build the frame there anyway, the frame base being ctx.kernel_sp and no
 * function of the sp the thread chose. switch.S asserts its own F_S2 against this. */
#define KICKOS_RV_TRAP_F_S2 64

/* Byte offset of the slot carrying the sp .Lrestore leaves on. The 30 saved words reach 116,
 * so this sits in the 8 bytes the 16-byte-aligned frame size already had spare and the frame
 * did not grow. FOUR places write it and nothing else may: trap_entry's three builders (the
 * U-mode entry, .Ltrap_build, .Ltrap_nested) and arch_context_init, the only other code that
 * fabricates a frame. A fabricated frame that leaves it zero resumes on a null sp at the
 * first mret. */
#define KICKOS_RV_TRAP_F_SP 120

/* mcause for a machine software interrupt: bit 31 set, code 3. The one INTERRUPT cause whose
 * frame is a thread's saved context, so .Ltrap_from_m names it rather than treating every
 * interrupt alike. switch.S reads it as an li immediate. */
#define KICKOS_RV_MCAUSE_MSIP 0x80000003

/* struct arch_context field offsets the entry reads as plain words, and the frame slots the
 * switcher reads. switch.S .equ's from these and arch_rv32imac.cc static_asserts offsetof
 * against them, so a field inserted ahead of them breaks the build rather than leaving the
 * entry to read trace_tid as stack_hi. */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_RV_CTX_OFF_SP 0
#define KICKOS_RV_CTX_OFF_TRACE_TID 4
#define KICKOS_RV_CTX_OFF_STACK_LO 8
#define KICKOS_RV_CTX_OFF_STACK_HI 12
#define KICKOS_RV_CTX_OFF_KERNEL_SP 16
#else
#define KICKOS_RV_CTX_OFF_SP 0
#define KICKOS_RV_CTX_OFF_STACK_LO 4
#define KICKOS_RV_CTX_OFF_STACK_HI 8
#define KICKOS_RV_CTX_OFF_KERNEL_SP 12
#endif

#endif /* KICKOS_ARCH_RV_TRAP_STACK_H */
