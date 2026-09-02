/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RV64IMAC supervisor context switch, trap entry and syscall trap. stvec is DIRECT, so every
 * cause arrives at kickos_rv64_stvec and one frame format serves every path.
 *
 * SIE stays masked through the syscall dispatch and nothing here sets it: that is what keeps
 * every interrupt frame a resumable thread context on a stack that outlives the trap.
 *
 * SSCRATCH INVARIANT: outside the prologue, sscratch holds the trap-stack TOP, which is also
 * this hart's per-CPU block address (percpu.h). U-mode can neither read nor write it.
 *
 * sstatus.SUM stays clear, so a kernel dereference of a low-half pointer faults; the kernel
 * reaches memory a process owns through the kaccess seam (kickos/aspace.h).
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

/* Displacements into this hart's per-CPU block (percpu.h), static_asserted against offsetof in
   arch_rv64imac.cc. The block begins where the trap stack ends, so its address IS the value
   sscratch carries and no second base register exists. */
#define PERCPU_CTX_CURRENT 0
#define PERCPU_SWITCH_TO   8
#define PERCPU_ISR_DEPTH   16

/* Ends the kernel lock's span over the swap. CALL ONLY where the outgoing frame is parked and
   sp already names the incoming one: the call frame then lands on the incoming thread's own
   stack below its saved frame, and .Lrestore reloads every register the call is free to
   clobber. */
    .macro  SWITCH_UNLOCK
#if KICKOS_KERNEL_CORES > 1
    call    kickos_switch_unlock
#endif
    .endm

/* One cell of this hart's block, addressed. At one core the array's first row is this hart's,
   so the address is a link-time constant and the image carries no CSR read to find it. */
    .macro  PERCPU_CELL reg, disp
#if KICKOS_NUM_CORES > 1
    csrr    \reg, sscratch
    .if \disp != 0
    addi    \reg, \reg, \disp
    .endif
#else
    .option push
    .option norelax
    la      \reg, kickos_rv64_percpu + KICKOS_RV64_TRAP_STACK_SIZE + \disp
    .option pop
#endif
    .endm

/* Its own section so a W^X split of the kernel's half has a boundary here; virt_rv64.ld places
 * it with kernel text.
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
    /* The sscratch invariant is restored before any frame is built: a fault on the stores below
       would re-enter this entry, whose first instruction swaps sp with sscratch, and would land
       the nested handler on the U-mode sp. */
    csrr    t1, sscratch            /* t1 = the interrupted user sp */
    csrw    sscratch, sp            /* sscratch = trap top: invariant restored */
    PERCPU_CELL t0, PERCPU_CTX_CURRENT
    ld      t0, 0(t0)
    ld      t0, CTX_KERNEL_SP(t0)
    beqz    t0, .Ltrap_nokstack
    /* sp is still the trap top, t1 carries the sp to hand back, and the originals of t0/t1 are
       at -8(sp)/-16(sp). */
    addi    t0, t0, -FRAME
    sd      ra, F_RA(t0)            /* ra's original into the frame; ra is now scratch */
    sd      t1, F_SP(t0)            /* the sp .Lrestore srets on */
    ld      ra, -8(sp)              /* ra = t0's original, staged */
    ld      t1, -16(sp)
    mv      sp, t0
    mv      t0, ra
    j       .Ltrap_regs             /* .Ltrap_regs stores no ra: F_RA is written above */

/* A U-mode thread with no kernel block seated: a provisioning bug, this arch selecting
 * ARCH_KERNEL_STACKS_MANDATORY. sp is the trap top, so the report spends the trusted stack.
 */
.Ltrap_nokstack:
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    call    kickos_rv64_no_kernel_stack

/* Interrupted S-mode. An INTERRUPT stays on the sp it interrupted, its frame being one .Lintr
 * may hand to a booked switch; every other S-mode trap pops its frame and srets and takes the
 * trusted trap stack.
 *
 * The interrupted sp is tested against the trap stack first, and a yes means the reporter or
 * the dispatch itself faulted. A no is not yet an unrelated stack: a descent past the row's
 * bottom leaves an sp comparison cannot separate from a kernel stack lower in .bss, so the
 * EXCEPTION leg reads the row's canary before it accepts the sp.
 */
.Ltrap_from_s:
    /* t0 holds sstatus and t1 the SPP test; the originals of both are at -8(sp)/-16(sp). */
    csrr    t0, sscratch            /* t0 = the interrupted sp */
    sub     t0, sp, t0              /* trap top - interrupted sp */
    li      t1, KICKOS_RV64_TRAP_STACK_SIZE
    beqz    t0, 1f                  /* nothing runs with sp AT the top */
    bgtu    t0, t1, 1f              /* unsigned, so an sp above the top wraps out of range */
    j       .Ltrap_reentry
1:  csrr    t0, scause
    bltz    t0, .Ltrap_from_s_ctx   /* scause[63] = 1 is an interrupt */
    /* Read the canary: gone means the row was descended through. t0 and t1 are scratch, their
       originals still at -8(sp)/-16(sp), and the reload below is what .Ltrap_nested's F_SP store
       depends on. */
    li      t0, KICKOS_RV64_TRAP_STACK_SIZE
    sub     t0, sp, t0
    ld      t0, 0(t0)
    li      t1, KICKOS_RV64_TRAP_CANARY
    bne     t0, t1, .Ltrap_reentry
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    j       .Ltrap_nested

/* TERMINAL, and it must EMIT before it stops. Entered with sp still the trap-stack top, so
 * the row's canary is one subtraction away and picks the line.
 * Nothing below touches a stack, and a0 is clobbered because no resume follows.
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
    .asciz  "\r\nKickOS: a supervisor trap was taken and the trap row's canary is gone: the trap stack overflowed\r\n"
.Ltrap_from_s_ctx:
    ld      t1, -16(sp)
    ld      t0, -8(sp)
    csrrw   sp, sscratch, sp        /* sp = interrupted sp; sscratch = trap top */
    j       .Ltrap_build

/* This frame is resumed on the stack it sits on, so F_SP is its own base plus FRAME. */
.Ltrap_build:
    addi    sp, sp, -FRAME
    sd      ra, F_RA(sp)
    addi    ra, sp, FRAME
    sd      ra, F_SP(sp)
    j       .Ltrap_regs

/* The frame goes on the trusted trap stack and the interrupted sp is only remembered. F_SP here
 * IS the -16(sp) slot the prologue stashed t1 in; sound only because .Ltrap_from_s has already
 * reloaded that word into t1, so do not move the reload after this store.
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

    /* gp is U-mode writable, so the canonical value goes in before any call. Out of a word
       because a PC-relative materialisation of an app-half symbol does not reach. */
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

/* The psABI put nr and a0..a3 in a0..a4 and the frame holds them. The result goes into the
 * frame because .Lrestore reloads a0 from there. ecall has no compressed form, so sepc + 4.
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

/* Every interrupt cause. The depth bracket is what makes arch_in_isr honest, and a switch
 * decided inside the handler is BOOKED: this frame already IS the outgoing thread's saved
 * context, and the outgoing context is read from the block's ctx_current, so an ISR that
 * rescheduled twice still saves THIS frame.
 */
.Lintr:
    PERCPU_CELL t0, PERCPU_ISR_DEPTH
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    mv      a0, sp
    call    kickos_rv64_isr_dispatch
    PERCPU_CELL t0, PERCPU_ISR_DEPTH
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    PERCPU_CELL t0, PERCPU_SWITCH_TO
    ld      t1, 0(t0)
    beqz    t1, .Lrestore
    sd      zero, 0(t0)             /* consume the booking before it can be seen again */
    PERCPU_CELL t0, PERCPU_CTX_CURRENT
    ld      t2, 0(t0)
    sd      sp, CTX_SP(t2)          /* this frame is the outgoing thread's saved context */
    sd      t1, 0(t0)               /* publish the incoming context */
    ld      sp, CTX_SP(t1)          /* stand on the incoming thread's frame */
    SWITCH_UNLOCK
    j       .Lrestore

/* A non-zero return means fault isolation claimed this fault and the reporter has rewritten the
 * frame's sepc, sstatus and F_SP. On zero the reporter never returns and the spin is a backstop.
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
    /* gp and tp are both U-mode writable and absent from the frame, so every resume forces them
       back to their canonical values. */
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
 *
 * THE CALLER MUST HAVE INTERRUPTS MASKED. arch_switch has already published `to` in the
 * block's ctx_current, so an interrupt before the store of `from` below reaches .Lintr and
 * books a swap that records a pointer into `from`'s stack as `to`'s saved context.
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
    sd      sp, CTX_SP(a0)
    ld      sp, CTX_SP(a1)          /* stand on the incoming thread's frame */
    SWITCH_UNLOCK
    j       .Lrestore

/* void kickos_rv64_start(struct arch_context* first) */
    .global kickos_rv64_start
kickos_rv64_start:
    ld      sp, CTX_SP(a0)
    j       .Lrestore

/* A link-time word: kernel text cannot reach __global_pointer$ PC-relatively. */
    .balign 8
.Lgp_anchor:
    .quad   __global_pointer$

/* arch_syscall(nr, a0, a1, a2, a3): args already in a0-a4 per the psABI.
 *
 * .apptrap because an unprivileged thread FETCHES this leaf and virt_rv64.ld selects the kernel
 * half by archive.
 */
    .section .apptrap, "ax"
    .global arch_syscall
    .global arch_syscall64
arch_syscall:
arch_syscall64:
    ecall
    ret

    .end
