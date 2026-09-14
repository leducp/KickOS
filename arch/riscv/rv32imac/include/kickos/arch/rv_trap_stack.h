/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The geometry trap_entry (switch.S) requires of the stacks it builds frames on: the
 * interrupted thread's own KERNEL stack for a U-mode entry and for the two M-mode causes that
 * keep the interrupted sp, and the trusted per-hart trap stack for every other M-mode trap.
 *
 * NO TRAP FIGURE HERE RESERVES AN UNPRIVILEGED THREAD'S STACK. A U-mode entry loads
 * ctx.kernel_sp and builds the frame there, so all the entry does with the sp a U-mode thread
 * chose is remember it in F_SP and test it, 16-byte aligned and inside [stack_lo, stack_hi],
 * because .Lrestore loads that word back and mret resumes U-mode on it. So the TRAP and SYS
 * figures below are a requirement on KICKOS_KERNEL_STACK_SIZE.
 *
 * TWO CASES STILL PRICE A THREAD STACK, neither a trap prologue. KICKOS_RV_TRAP_NEED_SYSPRIV,
 * a PRIVILEGED thread's syscall: its ecall traps with mstatus.MPP=M, so .Ltrap_from_m_ctx
 * keeps the frame on the sp it interrupted and the dispatch runs on that thread's own stack.
 * It is the larger term in every posture, so KICKOS_MIN_STACK_SIZE is set by it, and no bound
 * on an M-mode sp exists to refuse anything with: a floor below it buys a privileged thread
 * overflowing its own stack. And KICKOS_RV_TRAP_KERNEL_DEPTH_RET, kickos_thread_return, an
 * entry return with no fault and no redirect to relocate it; the other two death-path stubs
 * land on the KERNEL BLOCK, kickos_fault_stack_top answering with ctx.kernel_sp, as EXITK.
 *
 * tests/static/check_trap_redzone.sh re-measures the depth figures under
 * -fcallgraph-info=su,da and fails when a worst-case path exceeds what they enforce, when a
 * kernel-stack class's frame plus depth exceeds the block, or when a thread-stack class's
 * exceeds KICKOS_MIN_STACK_SIZE.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* KICKOS_BENCH selects one of the SYSPRIV figures below, an add_compile_definitions knob that
   reaches every C and every .S as 0 or 1. Deliberately NO fallback #define: with
   -Wundef -Werror an image that lost the definition fails to build, where a fallback would
   silently reserve the smaller figure. check_trap_redzone.sh resolves this ladder through the
   compiler for the same reason. */

/* The save frame trap_entry builds below the stack top it picked: `addi sp, sp, -128`, then
 * 30 word stores spanning 0(sp)..116(sp) plus the F_SP slot at 120. Equals FRAME in switch.S
 * and FRAME_WORDS*4 in arch_rv32imac.cc, both of which assert against it. */
#define KICKOS_RV_TRAP_FRAME 128

/* Worst-case bytes the kernel's C dispatch descends BELOW the frame, per trap class. Measured
 * (MinSizeRel, -fcallgraph-info=su,da) as the longest weighted path over the merged call
 * graph, rooted at the symbols switch.S branches to.
 *
 * NEITHER COUNTS THE PANIC REPORTER. kpanic leaves the interrupted stack before it prints
 * (kickos_panic_stack_enter, switch.S), so an assert anywhere in the dispatch costs these
 * figures its call site and nothing under it, and what the reporter descends is the gate's
 * PANIC class instead.
 *
 * BOTH ARE DELIBERATELY ABOVE THEIR MEASUREMENT, on the terms KICKOS_RV_TRAP_NESTED_DEPTH
 * already carries: the enforced figure is what a FUTURE change is measured against, and at
 * exactly the measurement one added assert anywhere in the dispatch is a gate failure on
 * whichever board happens to be deepest. Do NOT tighten them. What that would buy, stated here
 * so it is not re-derived as a new opportunity: the block falls from 1184 to 1104, 80 bytes
 * per KICKOS_THREAD_SLOTS.
 *
 *   _TRAP  480 enforced, 224 measured on the five non-bench presets and 256 on the two
 *              bench ones, one chain for the whole arch: kickos_isr_timer[0]
 *              -> ktime_on_timer[64] -> endpoint_wait_abort[32] -> sched::wake[16]
 *              -> resched_after_wake[32] -> reschedule[32] -> the SchedPolicy hook
 *              -> policy_on_switch_in[16] -> arm_slice[32]. The 32 KICKOS_BENCH adds is its
 *              IrqLock bracket taking a slot in sched::wake and one in reschedule.
 *   _SYS   912 enforced, 720 measured on BOTH bench presets, the dispatch that prints, and
 *              the two reach that figure down different tails:
 *              syscall_dispatch[80] -> bench_irq_sweep[112] -> dist_print_fmt[32]
 *              -> kprintf_paced[320] -> kconsole_write[0] -> console_emit[48]
 *              -> arch_console_write[0] -> console_tx_insert_line[64]
 *              -> console_write_line_sync[32] -> arch_console_write_sync[32]
 *              on qemu-riscv-bench, and dist_print_fmt[48] -> kprintf_paced[320]
 *              -> kvsnprintf[80] -> emit_uint[80] on esp32c6-wroom-bench, where the
 *              formatter beats the console. Off KICKOS_BENCH the deepest reading is 656.
 *
 * FRAME_SYS + _SYS = 1168, and the LOWEST word of a block is its overflow canary
 * (kernel/thread/thread.cc), so KICKOS_KERNEL_STACK_SIZE is 1184 here: 12 bytes of slack
 * above the canary word.
 *
 * The TRAP chain runs through an INDIRECT call, the SchedPolicy hook table;
 * tests/static/trap_redzone_indirect.txt binds each such site to the one slot that call
 * reaches, and the gate refuses to answer at all while a reachable site is unbound. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 480
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 912

/* THE SAME SYSCALL DISPATCH ON A THREAD STACK: a PRIVILEGED thread's ecall arrives with
 * mstatus.MPP=M, so .Ltrap_from_m_ctx keeps its frame on the sp it interrupted and
 * svc_trampoline runs the dispatch on that thread's own stack.
 *
 * A SEPARATE MACRO FROM _SYS FOR THE ROUNDING AND THE POSTURE, not for the roots, which are
 * the same set and measure the same bytes: a thread-stack figure is rounded up to the next
 * multiple of 64 and a kernel-block one is not.
 *
 * TWO FIGURES BECAUSE KICKOS_BENCH ADDS A SYSCALL ARM AND NOTHING ELSE COMPILES IT. One
 * fleet-wide number would make every non-bench board's floor reserve for a bracket its image
 * does not contain.
 *
 *   KICKOS_BENCH 0, 704 over 656 measured on qemu-riscv, tied by esp32c6-wroom-st. THE
 *   PRESETS DO NOT AGREE: gcc inlines syscall_body into syscall_dispatch on every one of them
 *   and that head frame is per board, so esp32c6-wroom reads 608; the two flat presets take a
 *   different chain again and read 544.
 *     656  syscall_dispatch[128] -> thread_create_call[272] -> thread_create[96]
 *          -> task_for[16] -> domain_for[32] -> grant_region_admissible[32]
 *          -> grant_hits_reserved[80]
 *
 *   KICKOS_BENCH 1, 832 over 720 measured on BOTH bench presets. The bench arm prints, so it
 *   walks the console, and the two boards reach one figure down two tails:
 *     720  syscall_dispatch[80] -> bench_irq_sweep[112] -> dist_print_fmt[32]
 *          -> kprintf_paced[320] -> kconsole_write[0] -> console_emit[48]
 *          -> arch_console_write[0] -> console_tx_insert_line[64]
 *          -> console_write_line_sync[32] -> arch_console_write_sync[32]
 *                                                          (qemu-riscv-bench)
 *     720  syscall_dispatch[80] -> bench_irq_sweep[112] -> dist_print_fmt[48]
 *          -> kprintf_paced[320] -> kvsnprintf[80] -> emit_uint[80]
 *                                                          (esp32c6-wroom-bench)
 *
 * A thread-stack figure here is its measurement rounded up to the next multiple of 64, so the
 * slack cannot be spent silently. 704 is that of 656; 832 is enforced above the 720.
 *
 * THE RESIDUAL THIS ONCE CARRIED IS GONE. A kernel assertion firing while a privileged thread
 * sat at the very bottom of a floor-sized stack had the console writer descend up to 144 bytes
 * below stack_lo, privileged; the reporter runs on an array of its own and touches this stack
 * nowhere. */
#if KICKOS_BENCH
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV 832
#else
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV 704
#endif

/* THE ONE FIGURE A PRIVILEGED THREAD'S STACK HAS TO HOLD, resolved by the posture above so
 * that the C floor assert and any reporter read the number the gate compares against. Nothing
 * refuses it at run time: an M-mode ecall carries no sp the entry may bound. */
#define KICKOS_RV_TRAP_NEED_SYSPRIV \
    (KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV)

/* THE FRAME TERM OF THE DEATH PATH IS THE MSIP FRAME, 128, AND IT IS NOT 0. The relocating
 * stubs start from the stack TOP, but this prices what a preemption puts BELOW the deepest
 * byte: the stub's exit_current reschedules, arch_switch pends msip, and an msip trap taken
 * from M-mode keeps its KICKOS_RV_TRAP_FRAME on the interrupted sp.
 *
 * A PLAIN INTEGER and not KICKOS_RV_TRAP_FRAME, because check_trap_redzone.sh scrapes this
 * macro as an immediate and REFUSES a figure it cannot find as one. arch_rv32imac.cc asserts
 * the two agree. */
#define KICKOS_RV_TRAP_NEST_EXIT 128

/* The two death-path stubs that relocate, on the thread's own KERNEL BLOCK: .Lfault moves sp
 * to kickos_fault_stack_top and arch_ctx_redirect fabricates a frame there, both answering
 * with ctx.kernel_sp. 432 on both bench presets and 384 on the five that are not, the
 * CAPABILITY TEARDOWN winning and not the fault reporter:
 *   kickos_thread_fault_exit[16] -> sched::exit_current[80] -> cap_teardown[32]
 *   -> teardown_entry[64] -> obj_close_protocol[32] -> mutex_force_unlock[32]
 *   -> sched::wake[16] -> resched_after_wake[32] -> reschedule[32]
 *   -> the SchedPolicy hook -> policy_on_switch_in[16] -> arm_slice[32]
 * The 48 KICKOS_BENCH adds is its IrqLock bracket taking a slot in cap_teardown, sched::wake
 * and reschedule.
 * NEVER BINDS, WHICH IS WHY IT IS ROUNDED LIKE A THREAD-STACK FIGURE. A kernel-block figure
 * is normally left at its measurement because it sizes KICKOS_KERNEL_STACK_SIZE and a byte
 * there costs KICKOS_THREAD_SLOTS; this class sizes nothing, _SYS winning the block on every
 * registered preset, so the reason for that convention does not reach it. 576 stands above
 * the 432 measured: 128 + 576 = 704 against 1180 usable, where _SYS asks 1168, so this would
 * have to grow 464 more before it bound. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK 576

/* kickos_thread_return ALONE: a PRIVILEGED thread's entry-return stub, a user thread's being
 * the kickos_user_thread_return syscall instead, so no fault and no redirect relocates it and
 * it runs at the depth the entry returned from on the thread's own stack. 384 on every
 * registered non-bench preset; KICKOS_MIN_STACK_SIZE is set by NEED_SYSPRIV and not by this
 * class, so the 128 + 384 red zone is checked against the floor rather than setting it.
 *
 * TWO FIGURES FOR THE SAME REASON _SYSPRIV CARRIES TWO. Under KICKOS_BENCH the IrqLock bracket
 * samples the outermost masked window inline, which costs one stack slot in sched::wake,
 * sched::reschedule and cap_teardown: 432 measured on BOTH rv32 bench presets, down the same
 * teardown chain _EXITK walks, entered from kickos_thread_return. One fleet-wide figure would
 * make every non-bench board's floor reserve for a bracket its image does not contain. This is
 * the one class sitting exactly on its bound off KICKOS_BENCH. */
#if KICKOS_BENCH
#define KICKOS_RV_TRAP_KERNEL_DEPTH_RET 448
#else
#define KICKOS_RV_TRAP_KERNEL_DEPTH_RET 384
#endif

/* THE SYSCALL REQUIREMENT HOLDS TWO FRAMES, the second being the msip frame the deferred
 * switcher builds. A blocking dispatch pends msip and the trap fires at whatever depth the
 * dispatch had reached; a tick's wake pends msip too, and that one fires on the mret back
 * INTO the dispatch, as deep as the dispatch ever goes. That frame is the outgoing thread's
 * saved CONTEXT (.Lswitch stores it as outgoing->sp), so .Ltrap_from_m_ctx keeps it on the
 * interrupted sp, which during a dispatch is the kernel stack the ecall frame is already on.
 *
 * A plain 256 and not 2 * KICKOS_RV_TRAP_FRAME: the gate scrapes this macro as an integer
 * immediate. arch_rv32imac.cc asserts the two agree.
 *
 * The requirement is FRAME + d + FRAME + s, for a dispatch depth d and the switcher's own
 * descent s below the frame it saved, so it is exact while s == 0, which the gate's SWITCH
 * class re-measures. */
#define KICKOS_RV_TRAP_FRAME_SYS 256

/* Bytes .Lswitch's own C descends BELOW the frame it saved, over kickos_arch_mpu_commit plus
 * kickos_trace_switch_done and kickos_bench_switch_done where their options are on. MEASURED
 * 0 on both boards.
 *
 * ZERO IS THE PART THAT MATTERS, and not for the requirement above. .Lswitch and arch_start
 * call that hook with sp = incoming->sp, and for a thread that has never trapped that sp is
 * still the frame arch_context_init fabricated on its USER stack: no privileged C frame on
 * user memory holds only because the hook is frameless. */
#define KICKOS_RV_TRAP_SWITCH_DEPTH 0

/* The trusted per-hart trap stack (g_rv_trap_stack, arch_rv32imac.cc). ONE frame: MIE is 0
 * from the vector to the mret, so no interrupt nests on a trap-stack frame. A synchronous
 * FAULT does nest and reuses this same frame slot, which is sound because such a fault is a
 * kernel bug: arch_fault_is_user_thread refuses a frame not on the running thread's kernel
 * stack, so kickos_rv_fault_report terminates and the overwritten frame is never resumed.
 *
 * .Lfault's reporter sizes this stack, the four .Lintr ISR arms being shallower:
 *   560  kickos_rv_fault_report[32] -> kickos_isr_fault[32] -> kprintf[320]
 *        -> kconsole_write[0] -> console_emit[48] -> arch_console_write[0]
 *        -> console_tx_insert_line[64] -> console_write_line_sync[32]
 *        -> arch_console_write_sync[32]                           (qemu-riscv-bench ALONE)
 *   544  every other registered preset. On the esp32c6 ones the same reporter wins down the
 *        FORMATTER instead of the console, kprintf[320] -> kvsnprintf[80] -> emit_uint[80].
 *
 * ENFORCED TWICE, over the same reporter chain on two different stacks: an ACCEPTED U-mode
 * fault runs .Lfault with sp on the frame the entry built, which is the kernel stack, so the
 * gate's FAULT class charges KICKOS_RV_TRAP_FRAME plus this depth, 960, against the block.
 *
 * THIS CLASS IS THE ONE THAT WALKS THE CONSOLE, so a console change moves it. 832 is
 * deliberately above the 560 measured: the enforced figure is what a FUTURE change is
 * measured against, and the margin costs one shared kernel array rather than per-thread
 * bytes. Do NOT tighten it back to the measurement. */
#define KICKOS_RV_TRAP_NESTED_DEPTH 832
#define KICKOS_RV_TRAP_STACK_SIZE \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH)

/* Alignment the entry requires of an interrupted U-mode sp. The RISC-V psABI keeps sp 16-byte
 * aligned, arch_context_init aligns the stack top to 16 and FRAME is a multiple of it, so
 * every legitimate sp satisfies it.
 *
 * It is enforced because .Lrestore hands that word back to mret: a U-mode thread resumed on a
 * misaligned sp takes its next stack access as a misaligned trap, on a core that traps those,
 * and re-enters the entry with the same sp for as long as it is scheduled. The ESP32-C6 traps
 * misaligned stores; QEMU virt completes them. The same figure divides
 * KICKOS_KERNEL_STACK_SIZE, so every slot's top is a legal frame base. */
#define KICKOS_RV_TRAP_SP_ALIGN 16

/* Byte offset of the s2 slot from the frame base. Exported for the faultsurvive kwrite arm,
 * which aims a U-mode sp so that THIS slot's store would land on the kernel witness word: it
 * needs the offset and the frame size together. switch.S asserts its own F_S2 against this. */
#define KICKOS_RV_TRAP_F_S2 64

/* Byte offset of the slot carrying the sp .Lrestore leaves on. The 30 saved words reach 116,
 * so this sits in the 8 bytes the 16-byte-aligned frame size already had spare. FOUR places
 * write it and nothing else may: trap_entry's three builders (the U-mode entry, .Ltrap_build,
 * .Ltrap_nested) and arch_context_init. A fabricated frame that leaves it zero resumes on a
 * null sp at the first mret. */
#define KICKOS_RV_TRAP_F_SP 120

/* mcause for a machine software interrupt: bit 31 set, code 3. The one INTERRUPT cause whose
 * frame is a thread's saved context. switch.S reads it as an li immediate. */
#define KICKOS_RV_MCAUSE_MSIP 0x80000003

/* struct arch_context field offsets the entry reads as plain words. switch.S .equ's from
 * these and arch_rv32imac.cc static_asserts offsetof against them, so a field inserted ahead
 * of them breaks the build rather than leaving the entry to read trace_tid as stack_hi. */
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

/* THE PANIC REPORTER'S OWN STACK, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed. NOTHING ABOVE REACHES IT: kpanic's console descent ends at a body the
 * callgraph walk cannot see through, so it is priced here and in no trap class.
 *
 * FRAME 0. The entry clears MIE before the move, so no interrupt lands here, and an EXCEPTION
 * taken inside the reporter re-enters trap_entry, which swaps sp with mscratch and builds its
 * frame on whatever that names: never this array. Such a fault is a kernel bug on a path that
 * is already terminal, exactly as for the nested frame on the trap stack above.
 *
 * THE DEPTH IS NOT THE MEASUREMENT. The PANIC class measures 208 at worst, on
 * qemu-riscv-bench, 192 on the other two qemu-riscv presets, and 160 to 176 on the four
 * esp32c6 ones. THE QEMU SIDE IS THE DEEPER ONE, which is the opposite of what the board
 * families do elsewhere: its chain runs one frame further, kputs -> kconsole_write
 * -> console_emit -> arch_console_write -> console_tx_insert_line -> console_write_line_sync
 * -> arch_console_write_sync[32], where the esp32c6 chain stops at console_write_line_sync.
 * 448 IS ENFORCED ABOVE THAT: a byte here costs one shared array rather than one per thread
 * slot, and the enforced figure is what a future change is measured against.
 * KICKOS_PANIC_STACK_SIZE (Kconfig) is what the array is cut to and what the gate compares
 * this against; arch_rv32imac.cc static_asserts the two agree. */
#define KICKOS_RV_PANIC_FRAME 0
#define KICKOS_RV_PANIC_DEPTH 448

#endif /* KICKOS_ARCH_RV_TRAP_STACK_H */
