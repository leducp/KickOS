/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RV64IMAC supervisor context switch, trap entry and syscall trap. kickos_rv64_init installs
 * stvec in DIRECT mode, so every cause arrives at kickos_rv64_stvec, and the entry saves the
 * FULL interrupted context (every GPR bar gp/tp, plus sepc and sstatus) before it demuxes on
 * scause. ONE frame format for every path, so a thread that trapped and a thread that
 * switched of its own accord resume through the same instructions.
 *   .Lecall     the syscall trap, from U-MODE ONLY. medeleg leaves ECALL-from-S undelegated
 *               (arch/riscv/chip/virt_rv64/startup.S), so cause 9 never arrives here at all.
 *               syscall_dispatch runs on the frame's own stack in privileged THREAD context,
 *               as arch.h requires.
 *   .Lintr      every interrupt cause, in ISR context, with a switch booked from the ISR
 *               performed by this leg's own exit rather than inside the handler.
 *   .Lfault     every remaining exception.
 *   arch_start  enters the first thread through the shared restore epilogue.
 *
 * SYSCALL DISPATCH RUNS WITH SIE MASKED, which the trap left clear and nothing here sets. That
 * is load-bearing rather than incidental: it is what keeps every interrupt frame a resumable
 * thread context standing on a stack that outlives the trap, which is what .Lintr's deferred
 * switch consumes. Unmasking it would put a third frame on a kernel block already carrying an
 * ecall frame and a dispatch nothing has measured.
 *
 * SSCRATCH INVARIANT: outside the prologue, sscratch holds the trap-stack TOP. That is what
 * makes the entry's first instruction a swap onto a trusted sp, and it holds through the
 * U-mode accept path (which writes it back rather than swapping) and through .Ltrap_nested.
 *
 * NO ROOT IS WRITTEN HERE. The image splits kernel text from app text into level-2 slots of
 * their own (arch/riscv/chip/virt_rv64/virt_rv64.ld), so one root serves both privilege levels
 * and satp changes only when the SPACE does, inside arch_aspace_activate. What that retires is
 * a root write and an sfence.vma on EVERY privilege transition, and with them the window in
 * which this epilogue ran privileged with a root the entry below would have refused.
 *
 * SSTATUS.SUM IS NEVER SET, so S-mode may not load or store a page carrying U and a kernel
 * dereference of a low-half pointer FAULTS. Nothing needs it: every kernel touch of memory a
 * process owns goes through the kaccess seam (kickos/aspace.h), which reaches the frame the
 * space's own tables name from the kernel's half. This epilogue writes sstatus WHOLE from the
 * frame, so a resumed U-mode thread gets whatever the frame carries and nothing is forced.
 *
 * gp AND tp ARE BOTH RE-ANCHORED, at .Ltrap_regs for the kernel C the entry calls and at
 * .Lrestore for the resume. Both are U-mode writable and neither is saved in the frame, so
 * without this a value a thread chose would outlive it. tp's canonical value is zero while no
 * ARCH_HAS_TLS is selected; a step that seats a thread pointer must re-seat it FROM THE CONTEXT
 * at these same two points, under #if KICKOS_TLS.
 *
 * A FAULT WHILE ALREADY ON THE TRAP STACK IS TERMINAL, and .Ltrap_from_s is where that is
 * decided. The prologue's first instruction puts sp back at the trap-stack top, so a fault
 * inside the dispatch or the reporter would rebuild the same frame forever with nothing on the
 * wire; comparing the interrupted sp against the row instead reaches the finisher with a line
 * naming which of the two it was.
 *
 * NOTHING HERE VALIDATES THE INTERRUPTED U-MODE SP, and that is sound rather than skipped: a
 * U-mode entry builds its frame on ctx.kernel_sp, so the sp the thread chose is only
 * remembered in F_SP and handed back by sret. A wild value costs the thread its own next
 * stack access and reaches no kernel memory.
 */

#include <kickos/arch/rv64_frame.h>

    .equ F_SEPC,     KICKOS_RV64_F_SEPC
    .equ F_SSTATUS,  KICKOS_RV64_F_SSTATUS
    .equ F_RA,       KICKOS_RV64_F_RA
    .equ F_T0,       KICKOS_RV64_F_T0
    .equ F_T1,       KICKOS_RV64_F_T1
    .equ F_T2,       KICKOS_RV64_F_T2
    .equ F_S0,       KICKOS_RV64_F_S0
    .equ F_S1,       KICKOS_RV64_F_S1
    .equ F_A0,       KICKOS_RV64_F_A0
    .equ F_A1,       KICKOS_RV64_F_A1
    .equ F_A2,       KICKOS_RV64_F_A2
    .equ F_A3,       KICKOS_RV64_F_A3
    .equ F_A4,       KICKOS_RV64_F_A4
    .equ F_A5,       KICKOS_RV64_F_A5
    .equ F_A6,       KICKOS_RV64_F_A6
    .equ F_A7,       KICKOS_RV64_F_A7
    .equ F_S2,       KICKOS_RV64_F_S2
    .equ F_S3,       KICKOS_RV64_F_S3
    .equ F_S4,       KICKOS_RV64_F_S4
    .equ F_S5,       KICKOS_RV64_F_S5
    .equ F_S6,       KICKOS_RV64_F_S6
    .equ F_S7,       KICKOS_RV64_F_S7
    .equ F_S8,       KICKOS_RV64_F_S8
    .equ F_S9,       KICKOS_RV64_F_S9
    .equ F_S10,      KICKOS_RV64_F_S10
    .equ F_S11,      KICKOS_RV64_F_S11
    .equ F_T3,       KICKOS_RV64_F_T3
    .equ F_T4,       KICKOS_RV64_F_T4
    .equ F_T5,       KICKOS_RV64_F_T5
    .equ F_T6,       KICKOS_RV64_F_T6
    .equ F_SP,       KICKOS_RV64_F_SP
    .equ FRAME,      KICKOS_RV64_FRAME
    .if F_SP <= F_T6
    .error "F_SP overlaps a saved GPR slot"
    .endif
    .if F_SP > FRAME - 8
    .error "F_SP lies outside the frame"
    .endif
    .if (FRAME % KICKOS_RV64_SP_ALIGN) != 0
    .error "FRAME is not a multiple of the psABI stack alignment"
    .endif

    .equ SSTATUS_SIE,  KICKOS_RV64_SSTATUS_SIE
    .equ SSTATUS_SPIE, KICKOS_RV64_SSTATUS_SPIE
    .equ SSTATUS_SPP,  KICKOS_RV64_SSTATUS_SPP

    .equ CTX_SP,          KICKOS_RV64_CTX_OFF_SP
    .equ CTX_KERNEL_SP,   KICKOS_RV64_CTX_OFF_KERNEL_SP

/* Its own section so a W^X split of the kernel's half (R2.3) has a boundary to draw between
 * the entry and the rest; virt_rv64.ld places it with kernel text and nothing else depends on
 * where it lands.
 */
    .section .text.privtrap, "ax"

/* stvec BASE: bits 1:0 hold the mode, so the base is 4-byte aligned. */
    .balign 4
    .global kickos_rv64_stvec
kickos_rv64_stvec:
    csrrw   sp, sscratch, sp        /* sp = trap top; sscratch = interrupted sp */
    sd      t0, -8(sp)              /* two scratch regs on the trusted stack */
    sd      t1, -16(sp)
    csrr    t0, sstatus
    andi    t1, t0, SSTATUS_SPP
    bnez    t1, .Ltrap_from_s
    /* Interrupted U-mode. No root goes in: the running space already maps the kernel's half,
       every space carrying the same level-2 entries.
       THE INVARIANT IS RESTORED BEFORE ANY FRAME IS BUILT. A fault on the stores below would
       re-enter this entry, whose first instruction swaps sp with sscratch, and would land the
       nested handler on the U-mode sp. */
    csrr    t1, sscratch            /* t1 = the interrupted user sp */
    csrw    sscratch, sp            /* sscratch = trap top: invariant restored */
    .option push
    .option norelax
    la      t0, kickos_rv64_ctx_current
    .option pop
    ld      t0, 0(t0)
    ld      t0, CTX_KERNEL_SP(t0)
    beqz    t0, .Ltrap_nokstack
    /* Accepted: the frame and the dispatch below it go on the thread's own kernel block. sp
       is still the trap top here, t1 carries the sp to hand back, and the originals of t0/t1
       are at -8(sp)/-16(sp). */
    addi    t0, t0, -FRAME
    sd      ra, F_RA(t0)            /* ra's original into the frame; ra is now scratch */
    sd      t1, F_SP(t0)            /* the sp .Lrestore srets on */
    ld      ra, -8(sp)              /* ra = t0's original, staged */
    ld      t1, -16(sp)
    mv      sp, t0
    mv      t0, ra
    j       .Ltrap_regs             /* .Ltrap_regs stores no ra: F_RA is written above */

/* A U-mode thread with no kernel block seated. The arch selects
 * ARCH_KERNEL_STACKS_MANDATORY and thread_create asserts the block, so this is a
 * provisioning bug and not a pointer the thread chose. sp is the trap top, so the report
 * spends the trusted stack.
 */
.Ltrap_nokstack:
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    call    kickos_rv64_no_kernel_stack

/* Interrupted S-mode, so the sp is the kernel's. It still needs a DECISION: a trap whose frame
 * IS a thread's saved context must stay on the sp it interrupted, which is a privileged
 * thread's own stack or the kernel block a dispatch was already descending. One cause is that:
 * any INTERRUPT, whose frame .Lintr may hand to a booked switch. Every other S-mode trap pops
 * its frame and srets, so it takes the trusted trap stack. The root is already the kernel one
 * at this level.
 *
 * THE INTERRUPTED sp IS TESTED AGAINST THE TRAP STACK FIRST, and this is the one path on which
 * the answer can be yes: nothing legitimate traps while running on that row. sstatus.SIE is 0
 * from the moment any trap is taken and neither the entry, the dispatch nor the reporter sets
 * it, so no interrupt lands there; ECALL-from-S is undelegated; and the privilege probe in
 * trap.S installs an stvec of its own and runs on the boot stack, never reaching here. So a
 * yes means the reporter or the dispatch itself faulted, or the row overflowed under it.
 */
.Ltrap_from_s:
    /* t0 holds sstatus and t1 the SPP test, and the originals of both are at -8(sp)/-16(sp),
       so both are scratch here and nothing is destroyed before it is saved. */
    csrr    t0, sscratch            /* t0 = the interrupted sp */
    sub     t0, sp, t0              /* trap top - interrupted sp */
    li      t1, KICKOS_RV64_TRAP_STACK_SIZE
    beqz    t0, 1f                  /* nothing runs with sp AT the top */
    bgtu    t0, t1, 1f              /* unsigned, so an sp above the top wraps out of range */
    j       .Ltrap_reentry
1:  csrr    t0, scause
    bltz    t0, .Ltrap_from_s_ctx   /* scause[63] = 1 is an interrupt */
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    j       .Ltrap_nested

/* TERMINAL, and it must EMIT before it stops: the frame this entry was about to build would be
 * the second one on a row that is already unusable. sp is still the trap-stack top, so the
 * row's low word is one subtraction away and the canary there tells an overflow from a plain
 * fault inside the reporter. Nothing below touches a stack, and a0 is clobbered because no
 * resume follows.
 */
.Ltrap_reentry:
    li      t0, KICKOS_RV64_TRAP_STACK_SIZE
    sub     t0, sp, t0
    ld      t0, 0(t0)
    li      t1, KICKOS_RV64_TRAP_CANARY
    la      a0, .Lreentry_msg
    beq     t0, t1, 1f
    la      a0, .Loverflow_msg
1:  j       kickos_rv64_supervisor_refuse

.Lreentry_msg:
    .asciz  "\r\nKickOS: a supervisor trap was taken while already on the trap stack: the fault reporter or the dispatch under it faulted\r\n"
.Loverflow_msg:
    .asciz  "\r\nKickOS: a supervisor trap was taken while already on the trap stack, and the row's canary is gone: the trap stack overflowed\r\n"
.Ltrap_from_s_ctx:
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    csrrw   sp, sscratch, sp        /* sp = interrupted sp; sscratch = trap top */
    j       .Ltrap_build

/* This frame is resumed on the stack it sits on, so F_SP is its own base plus FRAME, where
 * .Ltrap_nested's names a different stack. ra is free scratch from the moment its own slot
 * is written.
 */
.Ltrap_build:
    addi    sp, sp, -FRAME
    sd      ra, F_RA(sp)
    addi    ra, sp, FRAME
    sd      ra, F_SP(sp)
    j       .Ltrap_regs

/* The frame goes on the trusted trap stack and the interrupted sp is only remembered.
 * F_SP here IS the -16(sp) slot the prologue stashed t1 in, this frame starting one FRAME
 * below the trap-stack top. Sound only because .Ltrap_from_s has already reloaded that word
 * into t1; do not move the reload after this store.
 */
.Ltrap_nested:
    addi    sp, sp, -FRAME
    sd      ra, F_RA(sp)
    addi    ra, sp, FRAME
    csrrw   ra, sscratch, ra        /* sscratch = trap top; ra = interrupted sp */
    sd      ra, F_SP(sp)

.Ltrap_regs:
    sd      t0,  F_T0(sp)
    sd      t1,  F_T1(sp)
    sd      t2,  F_T2(sp)
    sd      s0,  F_S0(sp)
    sd      s1,  F_S1(sp)
    sd      a0,  F_A0(sp)
    sd      a1,  F_A1(sp)
    sd      a2,  F_A2(sp)
    sd      a3,  F_A3(sp)
    sd      a4,  F_A4(sp)
    sd      a5,  F_A5(sp)
    sd      a6,  F_A6(sp)
    sd      a7,  F_A7(sp)
    sd      s2,  F_S2(sp)
    sd      s3,  F_S3(sp)
    sd      s4,  F_S4(sp)
    sd      s5,  F_S5(sp)
    sd      s6,  F_S6(sp)
    sd      s7,  F_S7(sp)
    sd      s8,  F_S8(sp)
    sd      s9,  F_S9(sp)
    sd      s10, F_S10(sp)
    sd      s11, F_S11(sp)
    sd      t3,  F_T3(sp)
    sd      t4,  F_T4(sp)
    sd      t5,  F_T5(sp)
    sd      t6,  F_T6(sp)
    csrr    t0, sepc
    sd      t0, F_SEPC(sp)
    csrr    t0, sstatus             /* SIE is 0 here: the trap cleared it */
    sd      t0, F_SSTATUS(sp)

    /* gp IS U-MODE WRITABLE, so the canonical value goes in before any call and not only on
       the way out. -msmall-data-limit=0 on the KickOS-owned archives keeps kernel DATA out of
       small-data, but the linker still MAKES gp addressing out of any pair reaching within
       gp +/- 0x800, so this re-anchor and not that flag is what keeps the C below from being
       handed a gp a thread chose. Out of a word because a PC-relative materialisation of an
       app-half symbol does not reach. */
    .option push
    .option norelax
    ld      gp, .Lgp_anchor
    .option pop
    li      tp, 0                   /* same class as gp, and no TLS: canonical is zero */

    csrr    t0, scause
    bltz    t0, .Lintr              /* scause[63] = 1 is an interrupt */
    li      t1, KICKOS_RV64_CAUSE_ECALL_U
    beq     t0, t1, .Lecall
    j       .Lfault

/* The psABI put nr and a0..a3 in a0..a4 at the caller and the frame holds them. The result
 * comes back in a0 alone at this width, so the arch_syscall64 pair collapses; it goes into
 * the frame because .Lrestore reloads a0 from there and a result left only in the register
 * would be discarded by sret. ecall has no compressed form, so the user return is sepc + 4.
 */
.Lecall:
    ld      a0, F_A0(sp)
    ld      a1, F_A1(sp)
    ld      a2, F_A2(sp)
    ld      a3, F_A3(sp)
    ld      a4, F_A4(sp)
    call    syscall_dispatch
    sd      a0, F_A0(sp)
    ld      t0, F_SEPC(sp)
    addi    t0, t0, 4
    sd      t0, F_SEPC(sp)
    j       .Lrestore

/* Every interrupt cause. The depth bracket is what makes arch_in_isr honest, and it is why a
 * switch decided inside the handler is BOOKED rather than performed: performing it there would
 * save the outgoing thread's context pointing at a switch frame BELOW this one, and the global
 * depth would be decremented by whichever thread ran next.
 *
 * THE BOOKED SWAP IS THREE MEMORY ACCESSES AND NO FRAME WORK, because this frame already IS the
 * outgoing thread's saved context: .Ltrap_from_s_ctx and the U-mode accept path both put an
 * interrupt frame on a stack that outlives the trap. The outgoing context is read from
 * kickos_rv64_ctx_current rather than from anything the handler passed, so an ISR that
 * rescheduled twice still saves THIS frame.
 */
.Lintr:
    .option push
    .option norelax
    la      t0, g_rv64_isr_depth
    .option pop
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    mv      a0, sp
    call    kickos_rv64_isr_dispatch
    .option push
    .option norelax
    la      t0, g_rv64_isr_depth
    .option pop
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    .option push
    .option norelax
    la      t0, kickos_rv64_switch_to
    .option pop
    ld      t1, 0(t0)
    beqz    t1, .Lrestore
    sd      zero, 0(t0)             /* consume the booking before it can be seen again */
    .option push
    .option norelax
    la      t0, kickos_rv64_ctx_current
    .option pop
    ld      t2, 0(t0)
    sd      sp, CTX_SP(t2)          /* this frame is the outgoing thread's saved context */
    sd      t1, 0(t0)               /* publish the incoming context */
    ld      sp, CTX_SP(t1)
    j       .Lrestore

/* A NON-ZERO return means fault isolation claimed this fault and the reporter has already
 * rewritten the frame's sepc, sstatus and F_SP, so the resume lands in
 * kickos_thread_fault_exit at the top of the dying thread's kernel block. On zero the
 * reporter never returns and the spin is a backstop.
 */
.Lfault:
    mv      a0, sp
    call    kickos_rv64_fault_report
    bnez    a0, .Lrestore
1:  j       1b

/* --- Shared restore epilogue: pop the frame at sp, sret ---------------------
 * sstatus goes in BEFORE sepc: a trap taken between the two writes would overwrite sepc, and
 * the final sret would then jump back into this epilogue with the frame half popped.
 */
.Lrestore:
    /* Force gp to its canonical link-time value, the second of the two anchors: this one
       covers the RESUME, and the one in .Ltrap_regs covers the kernel C the entry calls.
       __global_pointer$ anchors the app's own small-data window, so a U-mode thread can write
       gp arbitrarily and the next thread's gp-relative loads would resolve at addresses it
       chose. Every resume funnels here, so gp is self-healing per switch.
       tp IS THE SAME CLASS and is forced beside it: U-mode writable, absent from the frame, and
       safe today only because no code in this port reads it. Zero is the canonical value while
       no ARCH_HAS_TLS is selected; a thread pointer seated per thread is re-seated FROM THE
       CONTEXT here, under #if KICKOS_TLS. */
    .option push
    .option norelax
    ld      gp, .Lgp_anchor
    .option pop
    li      tp, 0
    ld      t0, F_SSTATUS(sp)
    csrw    sstatus, t0
    ld      t0, F_SEPC(sp)
    csrw    sepc, t0
    ld      ra,  F_RA(sp)
    ld      t0,  F_T0(sp)
    ld      t1,  F_T1(sp)
    ld      t2,  F_T2(sp)
    ld      s0,  F_S0(sp)
    ld      s1,  F_S1(sp)
    ld      a0,  F_A0(sp)
    ld      a1,  F_A1(sp)
    ld      a2,  F_A2(sp)
    ld      a3,  F_A3(sp)
    ld      a4,  F_A4(sp)
    ld      a5,  F_A5(sp)
    ld      a6,  F_A6(sp)
    ld      a7,  F_A7(sp)
    ld      s2,  F_S2(sp)
    ld      s3,  F_S3(sp)
    ld      s4,  F_S4(sp)
    ld      s5,  F_S5(sp)
    ld      s6,  F_S6(sp)
    ld      s7,  F_S7(sp)
    ld      s8,  F_S8(sp)
    ld      s9,  F_S9(sp)
    ld      s10, F_S10(sp)
    ld      s11, F_S11(sp)
    ld      t3,  F_T3(sp)
    ld      t4,  F_T4(sp)
    ld      t5,  F_T5(sp)
    ld      t6,  F_T6(sp)
    /* The frame names the sp to leave on. Last, because every load above reads through the
       frame base. */
    ld      sp,  F_SP(sp)
    sret

/* void kickos_rv64_switch_now(struct arch_context* from, struct arch_context* to)
 * Thread context, so there is no trap frame to inherit and this one is synthesised: sepc is
 * the return address and sstatus the state to resume with, SPIE carrying the caller's SIE so
 * the sret puts back exactly the interrupt state the switch was decided under.
 *
 * THE CALLER MUST HAVE INTERRUPTS MASKED, and an enabled caller is not merely unusual here: the
 * store of `from` below is what makes this frame the outgoing thread's saved context, and
 * arch_switch has already published `to` as kickos_rv64_ctx_current. An interrupt in that
 * window reaches .Lintr with ctx_current naming `to`, and the booked swap would store a pointer
 * into `from`'s stack as `to`'s saved context.
 */
    .global kickos_rv64_switch_now
kickos_rv64_switch_now:
    addi    sp, sp, -FRAME
    sd      ra,  F_RA(sp)
    sd      ra,  F_SEPC(sp)         /* resume where our caller expects to continue */
    sd      t0,  F_T0(sp)
    sd      t1,  F_T1(sp)
    sd      t2,  F_T2(sp)
    sd      s0,  F_S0(sp)
    sd      s1,  F_S1(sp)
    sd      a0,  F_A0(sp)
    sd      a1,  F_A1(sp)
    sd      a2,  F_A2(sp)
    sd      a3,  F_A3(sp)
    sd      a4,  F_A4(sp)
    sd      a5,  F_A5(sp)
    sd      a6,  F_A6(sp)
    sd      a7,  F_A7(sp)
    sd      s2,  F_S2(sp)
    sd      s3,  F_S3(sp)
    sd      s4,  F_S4(sp)
    sd      s5,  F_S5(sp)
    sd      s6,  F_S6(sp)
    sd      s7,  F_S7(sp)
    sd      s8,  F_S8(sp)
    sd      s9,  F_S9(sp)
    sd      s10, F_S10(sp)
    sd      s11, F_S11(sp)
    sd      t3,  F_T3(sp)
    sd      t4,  F_T4(sp)
    sd      t5,  F_T5(sp)
    sd      t6,  F_T6(sp)
    addi    t0, sp, FRAME
    sd      t0,  F_SP(sp)
    csrr    t0, sstatus
    andi    t1, t0, SSTATUS_SIE
    slli    t1, t1, 4               /* SIE at bit 1 -> SPIE at bit 5 */
    li      t2, SSTATUS_SIE | SSTATUS_SPIE
    not     t2, t2
    and     t0, t0, t2
    or      t0, t0, t1
    li      t2, SSTATUS_SPP         /* SPP = supervisor: the sret stays at this level */
    or      t0, t0, t2
    sd      t0,  F_SSTATUS(sp)
    /* `from` is the context PHYSICALLY on the CPU here, this path running in thread context
       with interrupts masked by the caller. */
    sd      sp, CTX_SP(a0)
    ld      sp, CTX_SP(a1)
    j       .Lrestore

/* void kickos_rv64_start(struct arch_context* first)
 * The boot context is abandoned, as arch.h permits: the system ends through arch_shutdown
 * and never by unwinding to boot.
 */
    .global kickos_rv64_start
kickos_rv64_start:
    ld      sp, CTX_SP(a0)
    j       .Lrestore

/* The app's small-data anchor as a link-time word. Kernel text cannot materialise an app-half
 * address PC-relatively, the two halves being out of medany's reach of each other by
 * construction (virt_rv64.ld).
 */
    .balign 8
.Lgp_anchor:
    .quad   __global_pointer$

/* arch_syscall(nr, a0, a1, a2, a3): args already in a0-a4 per the psABI. arch_syscall64 is
 * the SAME trap, a 64-bit result returning in a0 alone at this width.
 *
 * .apptrap and not .text: virt_rv64.ld selects the kernel half by ARCHIVE, and an unprivileged
 * thread FETCHES this leaf, so it has to be linked into the app's half instead.
 */
    .section .apptrap, "ax"
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    ecall
    ret

    .end
