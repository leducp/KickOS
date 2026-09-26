/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The geometry trap_entry (switch.S) requires of the stacks it builds frames on: the
 * interrupted thread's own KERNEL stack for a U-mode entry and for the two M-mode causes that
 * keep the interrupted sp, and the per-hart trap stack for every other M-mode trap.
 *
 * NO TRAP FIGURE HERE RESERVES AN UNPRIVILEGED THREAD'S STACK: a U-mode entry builds its frame
 * on ctx.kernel_sp and only remembers and bounds the sp the thread chose, because mret resumes
 * on it. TWO DESCENTS STILL SPEND A THREAD STACK: a privileged thread's syscall (SYSPRIV), whose
 * ecall keeps the frame on the sp it interrupted, and kickos_thread_return (RET).
 *
 * The depths are the longest weighted path tests/static/check_trap_redzone.sh measures under
 * -fcallgraph-info=su,da; none counts the panic reporter, which kpanic reaches only through
 * kickos_panic_stack_enter and which the PANIC class prices on its own array.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* KICKOS_BENCH selects figures below and has NO fallback #define: with -Wundef -Werror an image
   that lost it fails to build, where a fallback would reserve the smaller figure. */

/* trap_entry's save frame: `addi sp, sp, -128`, 30 word stores plus the F_SP slot. switch.S and
 * arch_rv32imac.cc assert against it. */
#define KICKOS_RV_TRAP_FRAME 128

/* Deliberately ABOVE their measurement: the enforced figure is what a future change is measured
 * against. Do NOT tighten them; the block would fall from 1184 to 1104.
 *
 *   _TRAP  176 on the five non-bench presets, 208 on the two bench ones:
 *          kickos_isr_timer -> ktime_on_timer -> endpoint_wait_abort -> sched::wake
 *          -> pick_and_seat -> arch_ctx_redirect[32] -> arch_context_init[32]
 *   _SYS   784 on qemu-riscv-bench and 752 on esp32c6-wroom-bench, the arm that prints:
 *          syscall_dispatch[80]
 *          -> bench_irq_sweep[112] -> dist_print_fmt -> kprintf_paced[320] -> the console.
 *          704 off KICKOS_BENCH.
 *
 * FRAME_SYS + _SYS = 1168, and the lowest word of a block is its overflow canary, so
 * KICKOS_KERNEL_STACK_SIZE is 1184 here: 12 bytes above the canary word.
 *
 * The TRAP chain runs through the SchedPolicy hook table, bound per site in
 * tests/static/trap_redzone_indirect.txt. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 480
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 912

/* _SYS's dispatch on a PRIVILEGED thread's own stack. A separate macro for the rounding, a
 * thread-stack figure being rounded up to the next multiple of 64, and for the posture:
 * KICKOS_BENCH adds a syscall arm nothing else compiles.
 *
 *   KICKOS_BENCH 0, 704 on qemu-riscv and esp32c6-wroom-st, at its bound:
 *     syscall_dispatch[32] -> syscall_body[144] -> thread_create_call[272] -> thread_create[96]
 *     -> task_for -> domain_for -> grant_region_admissible -> grant_hits_reserved[80]
 *   gcc inlines syscall_body into syscall_dispatch per board: esp32c6-wroom reads 608, the two
 *   flat presets 544.
 *   KICKOS_BENCH 1, 784 on qemu-riscv-bench and 752 on esp32c6-wroom-bench, down the console. */
#if KICKOS_BENCH
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV 832
#else
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV 704
#endif

/* The one figure a privileged thread's stack has to hold. No bound on an M-mode sp exists to
 * refuse it at run time. */
#define KICKOS_RV_TRAP_NEED_SYSPRIV \
    (KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV)

/* The death path's frame term is the msip frame: a stub's exit_current pends msip, and an msip
 * trap from M-mode keeps its frame on the interrupted sp. A plain integer the gate scrapes;
 * arch_rv32imac.cc asserts it equals KICKOS_RV_TRAP_FRAME. */
#define KICKOS_RV_TRAP_NEST_EXIT 128

/* The fault and slay stubs on the thread's own KERNEL BLOCK. 384 on both bench presets, 352 on
 * the others, the fault stub's print or the capability teardown into the switch. NEVER BINDS:
 * _SYS wins the block on every preset, so it is rounded like a thread-stack figure. */
#define KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK 576

/* kickos_thread_return on a privileged thread's own stack. 352 on the non-bench presets, 384 on
 * the bench ones, where the IrqLock bracket and the phase marks cost three slots:
 *   kickos_thread_return -> exit_current[80] -> cap_teardown -> teardown_entry
 *   -> obj_close_protocol -> mutex_force_unlock -> sched::wake -> pick_and_seat
 *   -> arch_ctx_redirect[32] -> arch_context_init[32]
 * KICKOS_MIN_STACK_SIZE is set by NEED_SYSPRIV, not by this class. */
#if KICKOS_BENCH
#define KICKOS_RV_TRAP_KERNEL_DEPTH_RET 448
#else
#define KICKOS_RV_TRAP_KERNEL_DEPTH_RET 384
#endif

/* A syscall holds TWO frames: a blocking dispatch or a tick's wake pends msip, and that trap
 * keeps its frame on the kernel stack the ecall frame is already on. A plain integer the gate
 * scrapes; arch_rv32imac.cc asserts it is 2 * KICKOS_RV_TRAP_FRAME. Exact while
 * KICKOS_RV_TRAP_SWITCH_DEPTH is 0. */
#define KICKOS_RV_TRAP_FRAME_SYS 256

/* .Lswitch's own C below the frame it saved: 0 on every preset. It MUST stay 0: .Lswitch and
 * arch_start call their hook with sp = incoming->sp, which for a thread that has never trapped
 * is the frame arch_context_init fabricated on its USER stack. */
#define KICKOS_RV_TRAP_SWITCH_DEPTH 0

/* idle_entry's own frame, above the msip frame that lands on idle's stack: 16 on every
 * registered preset, rounded up like the other thread-stack figures. */
#define KICKOS_RV_TRAP_DEPTH_IDLE 64

/* The per-hart trap stack (g_rv_trap_stack). ONE frame: MIE is 0 from the vector to the mret.
 * A synchronous fault reuses the slot, which is sound because arch_fault_is_user_thread
 * refuses a frame off the running thread's kernel stack, so such a fault terminates.
 *
 * .Lfault's reporter sizes it: 576 on qemu-riscv-bench, kickos_rv_fault_report
 * -> kickos_isr_fault -> kprintf[320] -> the console; 544 elsewhere, down the formatter. An
 * accepted U-mode fault runs the same reporter on the kernel block, which the FAULT class
 * charges there. Deliberately above the measurement; do NOT tighten it. */
#define KICKOS_RV_TRAP_NESTED_DEPTH 832
#define KICKOS_RV_TRAP_STACK_SIZE \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH)

/* Alignment the entry requires of an interrupted U-mode sp: .Lrestore hands that word back to
 * mret, and the ESP32-C6 traps a misaligned store on every scheduling of such a thread. It also
 * divides KICKOS_KERNEL_STACK_SIZE, so every slot's top is a legal frame base. */
#define KICKOS_RV_TRAP_SP_ALIGN 16

/* The s2 slot's offset from the frame base, for the faultsurvive kwrite arm. switch.S asserts
 * its own F_S2 against it. */
#define KICKOS_RV_TRAP_F_S2 64

/* The slot carrying the sp .Lrestore leaves on. FOUR places write it and nothing else may:
 * trap_entry's three builders and arch_context_init. A fabricated frame that leaves it zero
 * resumes on a null sp at the first mret. */
#define KICKOS_RV_TRAP_F_SP 120

/* mcause for a machine software interrupt, the one interrupt cause whose frame is a thread's
 * saved context. */
#define KICKOS_RV_MCAUSE_MSIP 0x80000003

/* struct arch_context offsets switch.S reads as plain words; arch_rv32imac.cc static_asserts
 * offsetof against them. */
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

/* The panic reporter's own array. Frame 0: the entry clears MIE before the move, and an
 * exception inside the reporter re-enters trap_entry, which builds its frame elsewhere.
 * 224 on qemu-riscv-bench, 176 on the other qemu-riscv presets, 144 to 192 on esp32c6; 448 is
 * enforced above that, and arch_rv32imac.cc asserts KICKOS_PANIC_STACK_SIZE against the pair. */
#define KICKOS_RV_PANIC_FRAME 0
#define KICKOS_RV_PANIC_DEPTH 448

#endif /* KICKOS_ARCH_RV_TRAP_STACK_H */
