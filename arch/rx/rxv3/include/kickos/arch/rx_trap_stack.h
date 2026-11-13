/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The extents the RXv3 trap sites (switch.S) reserve: at the BOTTOM of a thread's own stack,
 * before they write a save through the live USP, and at the top of that thread's per-thread
 * KERNEL block, which the syscall trap's generic arm transfers to.
 *
 * AN EXTENT AND NOT A POINTER TEST. Every store these sites make through a live USP runs
 * SUPERVISOR, and the RX MPU checks user mode only (RX72M UM sec.17.1.1), so supervisor
 * bypasses it. A test that only asked whether the USP lies inside [stack_lo, stack_hi) would
 * let a user thread park it one word above stack_lo and have the kernel write supervisor into
 * the NEIGHBOUR's saved frame, there being no padding between pool stacks.
 *
 * NO PC OR PSW IS EVER SAVED ON A THREAD'S STACK. The PSW an RTE loads carries PM, so the pair a
 * thread resumes with lives in its context (ctx.resume_pc, ctx.resume_psw) or in the kernel
 * block's continuation header, and every RTE pops it off the ISP. What a save leaves on the
 * thread's stack is that thread's own registers.
 *
 * The classes, separated by WHICH STACK they land on:
 *   PENDSW    the deferred switcher's save on the interrupted thread's own stack; it is that
 *             thread's saved context and ctx.sp names it, so it does not move.
 *   SYS_FAST  the fastpath arm's save on the caller's own USP, that frame BEING the caller's
 *             saved context.
 *   SYSK      both C dispatches on the caller's kernel block.
 *   ISP       the interrupt stack, below: boot, every interrupt and exception nested on it, and
 *             every C call of the switch landing.
 * The generic syscall arm writes nothing on the thread's stack, so it has no class there.
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
 *     pushm r1-r15 (pendsw), or R15 and pushm r1-r14       60
 *     push.l x7 (FPSW, A1 gu/hi/lo, A0 gu/hi/lo)      88
 *     dpushm.d dr0-dr15                      128 ->  216
 *     dpushm.l dpsw-decnt                     12 ->  228
 *   switch.S checks this itself: FRAME_R1_OFF (168, the `add #FRAME_R1_OFF, r1` the fastpath
 *   arm emits) plus the 60 of R1-R15 is the 228 below.
 *
 * A DFPU-less build writes 88 rather than 228, so these are the -mdfpu worst case. */
#define KICKOS_RX_TRAP_FRAME_PENDSW 228
#define KICKOS_RX_TRAP_FRAME_SYS_FAST 228

/* Both frame builders bank the DPFPU file at a fixed multiple of this below the USP they arrive
 * on (switch.S refuses a frame where it is not), and a double at 4 mod 8 moves in two 64-bit
 * beats (RX72M UM sec.2.8.5, 16.2.1). arch_irq_restore and arch_syscall_reg align the USP to it
 * before a trap can bank, since only they can: the frame's top IS the resume USP. */
#define KICKOS_RX_TRAP_BANK_ALIGN 8

/* The continuation header the syscall trap's generic arm lays at the top T of the caller's
 * kernel block before it points the USP there: [T-16] a3, the fifth argument the psABI passes
 * on the stack, [T-12] userPC, [T-8] userPSW, [T-4] the USP the thread resumes on. A blocking
 * dispatch has the switcher freeze its continuation BELOW it, so it survives the block. */
#define KICKOS_RX_TRAP_SYSK_HEADER 16

/* THE SYSK CLASS, structural half: what stands between the top of a kernel block (T =
 * ctx.kernel_sp) and the first byte syscall_dispatch may use, plus what a preemption puts
 * below the deepest byte it uses.
 *
 *   -16    the continuation header above. No pad term: RX stack alignment is 4 bytes and
 *          every store here is a word store.
 *   -228   ONE WHOLE PENDSW ZONE, below the deepest byte of the dispatch. */
#define KICKOS_RX_TRAP_NEST_SYSK 244

/* Worst-case bytes the kernel's C dispatch descends BELOW each of those, per class, as the
 * longest weighted path over the merged call graph (MinSizeRel, -fcallgraph-info=su,da) from
 * the roots switch.S branches to. On rx-elf the compiler's reported frame ALREADY includes the
 * incoming return-address slot, so the chains below are not missing a per-level 4.
 *
 *   _PENDSW       0  the restore epilogue runs its C on the ISP, so nothing descends below an
 *                    incoming frame.
 *   _SYS_FAST     0  the same epilogue, kept a macro of its own because it is a distinct SITE
 *                    with its own guard.
 *   _SYSK       796  ENFORCED over 656 measured, as headroom. THE PANIC REPORTER IS NOT ON
 *                    THIS CHAIN: kpanic leaves this stack before it prints
 *                    (kickos_panic_stack_enter, switch.S). kickos_ipc_fastpath is the other
 *                    root and is dominated. rx72m sets the measurement, a task create of 9
 *                    grants seating its default capabilities; arch_rxv3.cc refuses more
 *                    than 9:
 *                    syscall_dispatch[48] -> task_create_call[136] -> spawn_locked[252]
 *                    -> cap_install_defaults[20] -> cap_seat_stdout[52] -> obj_ref_drop[28]
 *                    -> irq_ref_drop[28] -> irq_detach[12] -> irq_line_op[4]
 *                    -> arch_irq_mask[12] -> kickos_rx_group_arm[32]
 *                    -> kickos_rx_icu_line_arm[4] -> icu_ier_set[24] -> arch_irq_restore[4].
 *                    rx72m-flat reads 616, the same create, spawn_locked[212].
 *                    ONE FIGURE COVERS EVERY POSTURE, as rv32imac's _SYS does.
 *
 * ENFORCED figures are those measurements rounded up to 64, or above them as headroom where a
 * figure says so. _SYSK is NOT rounded, a kernel block being sized to it directly, so a byte of
 * slack there costs a byte per thread slot.
 *
 * rx72m is the only chip that selects ARCH_RXV3 and the gate runs on both of its presets,
 * so a figure here is the worst of two readings. A re-measurement MUST carry this board's
 * -misa=v3 -mdfpu baseline; at the compiler's default -misa=v1 with no DFPU every figure comes
 * out smaller than the truth. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW 0
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYS_FAST 0
/* THE ZONE IS 244 + 796 = 1040, which needs 1044 bytes of block once the canary word below it
 * is counted, and thread.cc rounds the block to KICKOS_STACK_ALIGN: 1056. So 12 of that
 * block's apparent slack is alignment fill and NOT room a depth may grow into. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_SYSK 796

/* THE EXIT CLASS, structural half: what a preemption puts below the deepest byte the DEATH
 * PATH uses. One whole PENDSW zone, which is KICKOS_RX_TRAP_REDZONE_PENDSW read from the other
 * end and what switch.S computes as BLOCK_USP_MAX_DROP = SIZE - REDZONE_PENDSW - 4. A FRAME
 * TERM and not 0: the stub runs at PSW.I = 1 with IPL = 0, which is what lets its exit_current
 * reschedule.
 *
 * PLUS THE EIGHT BYTES THE ENTRY SKIPS. arch_fault_redirect_to_exit seats the USP at
 * kernel_sp - 8, because kickos_rx_pendsw's block leg tests the distance with bleu and reads
 * a USP exactly at the top as zero distance, refusing it. Those 8 bytes sit above every byte
 * the descent uses, so the block holds them too: 8 + 228 is 236.
 *
 * A PLAIN INTEGER and not the expression, because check_trap_redzone.sh scrapes this macro as
 * an immediate and REFUSES a figure it cannot find as one. arch_rxv3.cc asserts the
 * composition instead. */
#define KICKOS_RX_TRAP_NEST_EXIT 236

/* The measured descent of the two stubs a dying thread runs PRIVILEGED on its own KERNEL
 * BLOCK, kickos_fault_stack_top answering with ctx.kernel_sp: kickos_thread_fault_exit and
 * kickos_thread_slay_exit. 416 on rx72m and rx72m-flat: the fault stub's record line descends
 * the timer rearm of the wake that ends its console write:
 *   kickos_thread_fault_exit[8] -> fault_record_print[56] -> kprintf_record[24]
 *   -> kvprintf_fault[108] -> console_emit[32] -> console_chip_writer_leave[8]
 *   -> console_dark_wake[24] -> wake[8]
 *   -> resched_after_wake[4] -> pick_and_seat[24] -> ktime_rearm[32] -> arch_timer_arm[24]
 *   -> arch_clock_now[32] -> __divdi3[32]
 *
 * Rounded like a thread-stack figure because it never binds: 236 + 448 = 684 against 1052
 * usable, where SYSK asks 1040. 448 is enforced over the 416 measured. */
#define KICKOS_RX_TRAP_KERNEL_DEPTH_EXITK 448

/* What each guard enforces: room below the USP, in bytes.
 *
 * The syscall figure is ONE constant covering both arms: the guard sits at the top of
 * kickos_rx_syscall_trap, above the `cmp #56, r1` that chooses the arm, so it runs before the
 * arm is known. The fastpath arm sets it; the generic arm writes nothing below the USP. */
#define KICKOS_RX_TRAP_REDZONE_PENDSW \
    (KICKOS_RX_TRAP_FRAME_PENDSW + KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW)
#define KICKOS_RX_TRAP_REDZONE_SYS 228

/* ENTRY_FRAME is spent ABOVE the validated USP: nothing, the INT trap and every interrupt being
 * accepted on the ISP. */
#define KICKOS_RX_TRAP_ENTRY_FRAME 0

/* What one kernel block has to hold: a requirement on KICKOS_KERNEL_STACK_SIZE and not a bound
 * anything refuses at run time, every byte of it being written by privileged code through a
 * pointer the kernel seated. arch_rxv3.cc static_asserts the block against it, and
 * check_trap_redzone.sh compares the same pair against the block the board configured. */
#define KICKOS_RX_TRAP_NEED_SYSK \
    (KICKOS_RX_TRAP_NEST_SYSK + KICKOS_RX_TRAP_KERNEL_DEPTH_SYSK)

/* Bytes the trap path reads ABOVE the USP the syscall guard validates, which the bound on that
 * USP has to reserve: the generic arm reads a3, the psABI fifth argument arch_syscall left at
 * [USP+4], so the read ends at USP+8. A bound that merely kept the USP AT OR BELOW stack_hi
 * would let that read run past the top of the stack. */
#define KICKOS_RX_TRAP_HEAD_ABOVE_SYS 8

/* Alignment the guards require of a live USP. Every store in both frame builds is a word store
 * (mov.l, push.l, pushm, and the DPFPU bank on top of them), and arch_context_init aligns the
 * stack top to 4. It is enforced because an in-bounds MISALIGNED USP makes every one of those
 * stores misaligned: on a core that traps them the nested trap re-enters the same prologue,
 * rebuilds one frame lower, faults again, and descends per iteration with no watchdog, so no
 * write lands and the kernel live-locks on an unprivileged thread's say-so. */
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
#define KICKOS_RX_CTX_OFF_RESUME_PC 20
#define KICKOS_RX_CTX_OFF_RESUME_PSW 24
#else
#define KICKOS_RX_CTX_OFF_SP 0
#define KICKOS_RX_CTX_OFF_STACK_LO 4
#define KICKOS_RX_CTX_OFF_STACK_HI 8
#define KICKOS_RX_CTX_OFF_KERNEL_SP 12
#define KICKOS_RX_CTX_OFF_RESUME_PC 16
#define KICKOS_RX_CTX_OFF_RESUME_PSW 20
#endif

/* THE PANIC REPORTER'S OWN STACK, which kickos_panic_stack_enter (switch.S) moves to before a
 * banner is printed. The walk stops at that body, so an assert on SYSK or EXITK costs
 * those figures its call site and nothing under it, and the panic console is priced here
 * instead. THE FAULT REPORTER IS A DIFFERENT CHAIN and is not priced here: it runs in thread
 * context on the dying thread's own block and is what EXITK still measures.
 *
 * FRAME 0. The entry clears PSW.I before the move and sets PSW.U, so R0 names the USP from
 * there on and an exception, which RX accepts on the ISP, lands on a stack this array does not
 * share.
 *
 * 200 MEASURED on rx72m and rx72m-flat, under an enforced 320, and what the reporter
 * descends is the UART RECLAIM's baud arithmetic, from its refusal arm:
 *   kickos_panic_report[8] -> refusal_report[28] -> kfault_terminate[20]
 *   -> kickos_terminal_outcome_apply[4] -> arch_reboot[12] -> kpanic_enter_unlocked[4]
 *   -> arch_console_reclaim[12] -> rx::reg::sci::baud_select[80] -> __divdi3[32]
 * KICKOS_PANIC_STACK_SIZE (Kconfig) cuts the array and is what the gate compares this against;
 * arch_rxv3.cc static_asserts the two agree. */
#define KICKOS_RX_PANIC_FRAME 0
#define KICKOS_RX_PANIC_DEPTH 320

/* IDLE'S OWN STACK, the USP idle_entry runs on privileged. Every interrupt is accepted on the
 * ISP, so what lands here is one whole PENDSW zone, the save a switch away from idle builds. A
 * plain integer the gate scrapes;
 * arch_rxv3.cc asserts it equals KICKOS_RX_TRAP_REDZONE_PENDSW.
 *
 * DEPTH is idle_entry and arch_idle_wait, 8 measured on every preset, rounded up to 64. */
#define KICKOS_RX_TRAP_NEST_IDLE 228
#define KICKOS_RX_TRAP_DEPTH_IDLE 64

/* THE INTERRUPT STACK, which the image links as _kernel_stack_size below _istack. Boot runs on
 * it, arch_start leaves boot's frames on it, and an interrupt and then an exception nest below
 * them (trap_redzone_roots.txt derives the levels). DEPTH is boot's deepest chain plus the
 * deepest root of each level: 1828 on rx72m, 1636 on rx72m-flat, against
 * the 8 KiB rx72m.ld links.
 *
 * FRAME: the PC and PSW an acceptance pushes, twice, and the R15 kickos_rx_pendsw still holds
 * on the ISP when it calls the refusal. The pair the restore stages for its RTE sits where the
 * acceptance's own pair sat. */
#define KICKOS_RX_ISP_FRAME 20
#define KICKOS_RX_ISP_DEPTH 2304

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class PENDSW   frame=KICKOS_RX_TRAP_FRAME_PENDSW depth=KICKOS_RX_TRAP_KERNEL_DEPTH_PENDSW
 * class SYS_FAST frame=KICKOS_RX_TRAP_FRAME_SYS_FAST depth=KICKOS_RX_TRAP_KERNEL_DEPTH_SYS_FAST
 * class SYSK     frame=KICKOS_RX_TRAP_NEST_SYSK depth=KICKOS_RX_TRAP_KERNEL_DEPTH_SYSK stack=kernel
 *
 * THE DEATH PATH. The backend rewrites the stacked PC so the RTE lands in a stub with R0 at
 * kickos_fault_stack_top, which answers with ctx.kernel_sp. EXITK is the two relocating stubs on
 * the block.
 *
 * class EXITK    frame=KICKOS_RX_TRAP_NEST_EXIT depth=KICKOS_RX_TRAP_KERNEL_DEPTH_EXITK stack=kernel
 * class PANIC    frame=KICKOS_RX_PANIC_FRAME depth=KICKOS_RX_PANIC_DEPTH stack=panic
 * class IDLE     frame=KICKOS_RX_TRAP_NEST_IDLE depth=KICKOS_RX_TRAP_DEPTH_IDLE stack=idle
 * class ISP      frame=KICKOS_RX_ISP_FRAME depth=KICKOS_RX_ISP_DEPTH stack=main
 */

#endif /* KICKOS_ARCH_RX_TRAP_STACK_H */
