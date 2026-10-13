/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The geometry trap_entry (switch.S) requires of the stacks it builds frames on: the
 * interrupted thread's own KERNEL stack for a U-mode entry and for the two M-mode causes that
 * keep the interrupted sp, and the per-hart trap stack for every other M-mode trap.
 *
 * NO TRAP FIGURE HERE RESERVES AN UNPRIVILEGED THREAD'S STACK: a U-mode entry builds its frame
 * on ctx.kernel_sp and only remembers and bounds the sp the thread chose, because mret resumes
 * on it.
 *
 * The depths are the longest weighted path tests/static/check_trap_redzone.sh measures under
 * -fcallgraph-info=su,da; none counts the panic reporter, which kpanic reaches only through
 * kickos_panic_stack_enter and which the PANIC class prices on its own array.
 */

#ifndef KICKOS_ARCH_RV_TRAP_STACK_H
#define KICKOS_ARCH_RV_TRAP_STACK_H

/* KICKOS_AMP_OWN_IMAGE selects figures below and has no fallback #define: with -Wundef -Werror
   an image that lost it fails instead of reserving a smaller figure. */

/* trap_entry's save frame: `addi sp, sp, -128`, 30 word stores plus the F_SP slot. switch.S and
 * arch_rv32imac.cc assert against it. */
#define KICKOS_RV_TRAP_FRAME 128

/* Deliberately ABOVE their measurement: the enforced figure is what a future change is measured
 * against. Do NOT tighten them; the block would fall from 1184 to 1104.
 *
 *   _TRAP  192 on the five non-bench presets, 224 on the two bench ones:
 *          kickos_rv_ext_dispatch -> console_tx_isr -> console_tx_room_freed
 *          -> console_dark_wake -> sched::wake -> pick_and_seat -> arch_ctx_redirect[32]
 *          -> arch_context_init[32]
 *   _SYS   768 on qemu-riscv-bench and 752 on esp32c6-wroom-bench, the arm that prints:
 *          syscall_dispatch[80]
 *          -> bench_irq_sweep[112] -> dist_print_fmt -> kprintf_paced[320] -> the console.
 *          576 off KICKOS_BENCH, 512 on the two flat presets.
 *
 * FRAME_SYS + _SYS = 1168, and the lowest word of a block is its overflow canary, so
 * KICKOS_KERNEL_STACK_SIZE is 1184 here: 12 bytes above the canary word.
 */
#define KICKOS_RV_TRAP_KERNEL_DEPTH 480
#define KICKOS_RV_TRAP_KERNEL_DEPTH_SYS 912

/* ENTRY_FRAME is spent ABOVE the validated sp, and a U-mode trap writes nothing on the thread's
 * own stack: trap_entry's csrrw moves to the trap stack before its first store. */
#define KICKOS_RV_TRAP_ENTRY_FRAME 0
#define KICKOS_RV_TRAP_NEED_USER 0

/* The death path's frame term is the msip frame: a stub's exit_current pends msip, and an msip
 * trap from M-mode keeps its frame on the interrupted sp. A plain integer the gate scrapes;
 * arch_rv32imac.cc asserts it equals KICKOS_RV_TRAP_FRAME. */
#define KICKOS_RV_TRAP_NEST_EXIT 128

/* The fault and slay stubs on the thread's own KERNEL BLOCK. C6 AMP node 0 measures 624
 * through its self-test diagnostic; the other non-bench nodes are smaller. _SYS still wins
 * the block, so this class does not set its size. */
#if KICKOS_AMP_OWN_IMAGE
#define KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK 640
#else
#define KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK 576
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
 * .Lfault's reporter sizes it: 560 on qemu-riscv-bench, kickos_rv_fault_report
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
 * 192 on qemu-riscv-bench, 160 on the other qemu-riscv presets, 144 to 160 on esp32c6; 448 is
 * enforced above that, and arch_rv32imac.cc asserts KICKOS_PANIC_STACK_SIZE against the pair. */
#define KICKOS_RV_PANIC_FRAME 0
#define KICKOS_RV_PANIC_DEPTH 448

/*
 * The trap classes tests/static/check_trap_redzone.sh measures against the figures above,
 * one line each; tests/static/trap_redzone_roots.txt says what each option means and roots
 * every class.
 *
 * class TRAP    frame=KICKOS_RV_TRAP_FRAME depth=KICKOS_RV_TRAP_KERNEL_DEPTH stack=kernel
 * class SYS     frame=KICKOS_RV_TRAP_FRAME_SYS depth=KICKOS_RV_TRAP_KERNEL_DEPTH_SYS stack=kernel
 * class FAULT   frame=KICKOS_RV_TRAP_FRAME depth=KICKOS_RV_TRAP_NESTED_DEPTH stack=kernel
 * class SWITCH  frame=KICKOS_RV_TRAP_FRAME depth=KICKOS_RV_TRAP_SWITCH_DEPTH
 * class NESTED  frame=KICKOS_RV_TRAP_FRAME depth=KICKOS_RV_TRAP_NESTED_DEPTH stack=trap
 * class EXITK   frame=KICKOS_RV_TRAP_NEST_EXIT depth=KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK stack=kernel
 * class PANIC   frame=KICKOS_RV_PANIC_FRAME depth=KICKOS_RV_PANIC_DEPTH stack=panic
 * class IDLE    frame=KICKOS_RV_TRAP_FRAME depth=KICKOS_RV_TRAP_DEPTH_IDLE stack=idle
 */

#endif /* KICKOS_ARCH_RV_TRAP_STACK_H */
