/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RXv3 context switch + syscall trap assembly (Renesas RX72M).
 *   1. arch_switch: record the target + pend the SWINT switcher. The physical swap
 *                   always happens in the SWINT handler, so ONE saved-frame format
 *                   serves both a voluntary block (thread context) and a preemptive
 *                   wake (ISR context).
 *   2. kickos_rx_pendsw: the deferred switcher, installed at INTB[SWINT]. Saves the
 *                   interrupted R1-R15, FPSW, the two accumulators and the DPFPU file on
 *                   the outgoing thread's USP and its PC and PSW in its context, then
 *                   restores the incoming one. SWINT carries the LOWEST interrupt
 *                   priority, so it is accepted only after every other ISR drains. Runs
 *                   with I=0, so g_arch_next is stable under it.
 *   3. arch_start: enter the first thread through the shared restore epilogue.
 *   4. kickos_rx_syscall_trap + svc_trampoline: INT #SYS vectors to the trap on the
 *                   ISP; it lays the return state on the CALLING THREAD'S KERNEL BLOCK
 *                   (ctx.kernel_sp), points the USP there and RTEs into svc_trampoline
 *                   supervisor on that block, so nothing is written through the pointer
 *                   the thread chose and a blocking dispatch's continuation is still
 *                   per-thread (arch.h arch_syscall). rx_trap_stack.h derives what each
 *                   stack holds.
 *   5. the INTB guards: every slot but the syscall's tells INT #n from an acceptance, and
 *                   kickos_rx_soft_trap takes the trap as a fault of the trapping thread.
 *
 * psABI: args in R1-R4, 5th+ on the stack ([SP+4] at entry, [SP]=return address), result in
 * R1, callee-saved R6-R13 (GCC RX preserves R6-R13, wider than the published R6-R11), R0=SP.
 * INT saves ONLY PC then PSW to the ISP (ISA UM sec.5.5.9), and RTE pops PC then PSW.
 * Interrupt acceptance switches to the ISP (U=0) and pushes the frame there regardless of
 * the interrupted stack.
 *
 * EVERY RTE HERE POPS FROM THE ISP, and the pair it pops is the hardware's own or a copy held in
 * kernel memory: the PSW an RTE loads carries PM, so a copy a thread or a sibling could write
 * would hand that thread supervisor. The resume USP is loaded first, with mvtc or as the USP a
 * clrpsw u leaves behind.
 *
 * I=1 IMPLIES U=1 EVERYWHERE IN arch/rx. I becomes 1 only from an RTE popping a word that carries
 * U too or from idle's WAIT on its USP, and every clrpsw u runs with I clear, so no interrupt is
 * accepted while R0 is the ISP.
 * kickos_rx_pendsw reads a PM=0 entry as supervisor-on-USP off that alone.
 *
 * Saved frame, low->high on the thread's USP (ctx.sp points at the lowest word = A0 low, the
 * DPFPU file below it under -mdfpu). Matches arch_context_init in arch_rxv3.cc:
 *     [+0 A0LO][+4 A0HI][+8 A0GU][+12 A1LO][+16 A1HI][+20 A1GU][+24 FPSW]
 *     [+28 R1 .. +84 R15]
 * The word ABOVE the frame ([+88]) is the address a returning entry() RTSes into
 * (kickos_thread_return, or kickos_user_thread_return for a user thread). The PC and PSW the
 * frame resumes with are ctx.resume_pc and ctx.resume_psw.
 *
 * RX disp[Rn] addressing takes an UNSIGNED displacement only: every stack edit here
 * adjusts the pointer first, never uses a negative displacement.
 */

#include <kickos/arch/rx_trap_stack.h>
#include <kickos/sys/image_offsets.h>
#include <kickos/board_config.h>
#include "chip_layout.h"

    .text

/* Rewinds the interrupted PC at 8[r0] to its image's _kickos_user_cas load when it lies in
   [cas_start, cas_store] of any header the image table names, and goes to .Lpendsw_ucas_out.
   Clobbers r14 and r15. */
    .macro UCAS_REWIND
    mov.l   #_g_kickos_ucas_span, r15
    mov.l   4[r15], r15            /* the highest store of any image */
    cmp     8[r0].l, r15           /* store - PC */
    bltu    .Lpendsw_ucas_out
    mov.l   #_g_kickos_ucas_span, r15
    mov.l   [r15], r15             /* the lowest load of any image */
    cmp     8[r0].l, r15           /* load - PC */
    bgtu    .Lpendsw_ucas_out
    mov.l   #___kickos_image_table, r14
.Lpendsw_ucas_walk:
    mov.l   #___kickos_image_table_end, r15
    cmp     r15, r14               /* cursor - end */
    bgeu    .Lpendsw_ucas_out
    mov.l   [r14], r15             /* the header */
    mov.l   KICKOS_IMAGE_HEADER_OFF_CAS_STORE[r15], r15
    cmp     8[r0].l, r15           /* store - PC */
    bltu    .Lpendsw_ucas_next
    mov.l   [r14], r15
    mov.l   KICKOS_IMAGE_HEADER_OFF_CAS_START[r15], r15
    cmp     8[r0].l, r15           /* load - PC */
    bgtu    .Lpendsw_ucas_next
    mov.l   r15, 8[r0]
#if defined(KICKOS_WITNESS_REWIND) && KICKOS_WITNESS_REWIND
    beq     .Lpendsw_ucas_out      /* already at the load: nothing moved */
    mov.l   #_g_kickos_witness_rewinds, r15
    mov.l   [r15], r14
    add     #1, r14
    mov.l   r14, [r15]
#endif
    bra     .Lpendsw_ucas_out
.Lpendsw_ucas_next:
    add     #4, r14
    bra     .Lpendsw_ucas_walk
    .endm

/* PSW bit masks (ISA UM sec.1.2.2.4): I=b16, U=b17, PM=b20. RX gas lexes a leading
 * "PSW"/"USP"/... as a control register, so these names avoid that. */
    .equ BIT_U,       0x00020000   /* PSW.U (select USP) */
    .equ MASK_NOT_PM, 0xffefffff   /* clear PSW.PM (-> supervisor) */
    .equ MASK_PM,     0x00100000   /* PSW.PM: 0 supervisor, 1 user */
    .equ ICU_SWINTR,  KICKOS_LAYOUT_ICU_BASE + 0x2E0 /* ICU.SWINTR: bit0 pends SWINT (vector 27) */
    .equ ICU_IR_SWINT, KICKOS_LAYOUT_ICU_BASE + 27    /* ICU.IR027: the SWINT request flag */
    .equ BIT_I,       0x00010000   /* PSW.I (interrupt enable) */
    .equ IPL_FIELD,   0x0f000000   /* PSW.IPL */
/* struct arch_context field offsets read by the trap sites. Taken from rx_trap_stack.h,
   which arch_rxv3.cc static_asserts offsetof against. */
    .equ F_CTX_STACK_LO, KICKOS_RX_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_RX_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_RX_CTX_OFF_KERNEL_SP
    .equ F_CTX_RESUME_PC, KICKOS_RX_CTX_OFF_RESUME_PC
    .equ F_CTX_RESUME_PSW, KICKOS_RX_CTX_OFF_RESUME_PSW
/* The continuation header the generic arm lays at the top T of the caller's kernel block:
     [T-16] a3, the psABI fifth argument syscall_dispatch reads at [SP+4]
     [T-12] the userPC and [T-8] the userPSW the trampoline returns with
     [T-4]  the USP the thread resumes on
   A blocking dispatch freezes its continuation BELOW it, so it survives the block. */
    .equ SYSK_HEADER, KICKOS_RX_TRAP_SYSK_HEADER
/* The deepest a USP may sit inside a kernel block and still be accepted by
   kickos_rx_pendsw, as a distance DOWN from the block's top because that is what the compare
   there holds. SIZE, less the lowest word (the overflow canary in kernel/thread/thread.cc,
   which a frame must not consume), less the save the switcher builds below the accepted USP
   plus the restore descent under it (KICKOS_RX_TRAP_REDZONE_PENDSW). Positive by
   arch_rxv3.cc's KICKOS_RX_TRAP_NEED_SYSK assert. */
    .equ BLOCK_USP_MAX_DROP, \
        KICKOS_KERNEL_STACK_SIZE - KICKOS_RX_TRAP_REDZONE_PENDSW - 4
/* Byte offset of the R1 slot from the frame base: the leaf's args, and the slot
   arch_ctx_set_syscall_result writes. arch_rxv3.cc asserts the same value. */
#if defined(__RX_DFPU_INSNS__)
    .equ FRAME_R1_OFF, 168
/* R1-R15 sit at and above the R1 slot, so the frame the guard prices is exactly 60 bigger.
   RX gas takes `.if <expr>` only, so the equality is written as a difference against zero. */
    .if FRAME_R1_OFF + 60 - KICKOS_RX_TRAP_FRAME_PENDSW
    .error "FRAME_R1_OFF disagrees with KICKOS_RX_TRAP_FRAME_PENDSW (rx_trap_stack.h)"
    .endif
/* dpushm.l dpsw-decnt, the three words below the DR bank. */
    .equ DPFPU_CTRL_BYTES, 12
    .if ((KICKOS_RX_TRAP_FRAME_PENDSW - DPFPU_CTRL_BYTES) | (KICKOS_RX_TRAP_FRAME_SYS_FAST - DPFPU_CTRL_BYTES)) & (KICKOS_RX_TRAP_BANK_ALIGN - 1)
    .error "the DR bank is not a multiple of KICKOS_RX_TRAP_BANK_ALIGN below the USP, so the USP alignment arch_irq_restore and arch_syscall_reg give it no longer aligns the bank"
    .endif
#else
    .equ FRAME_R1_OFF, 28
#endif

/* arch_switch(from, to): R1=from (ignored; the switcher saves g_arch_current),  */
/* R2=to. Called with the kernel IrqLock held (IPL>=SWINT), so SWINT is masked   */
/* and fires only once the lock releases or the ISR chain drains.                */
    .global _arch_switch
_arch_switch:
    mov.l   #_g_arch_next, r3
    mov.l   r2, [r3]           /* g_arch_next = to */
    mov.l   #ICU_SWINTR, r3
    mov.b   #1, [r3]           /* pend SWINT (deferred switch) */
    rts

/* arch_irq_restore(state): R1 = the IPL arch_irq_save returned. MVTIPL takes an immediate    */
/* only, so the whole PSW goes back via MVTC with only the IPL field (b27-b24, which          */
/* arch_rxv3.cc pins) replaced; flags, U and I are kept, and PM is ignored on write in        */
/* supervisor.                                                                                */
/*                                                                                            */
/* THE MVTC IS WHERE A SWITCH PENDED UNDER THE LOCK IS TAKEN, so R0 there is the USP the      */
/* switcher saves on, and the DPFPU file it banks starts 216 bytes below it. The operand bus  */
/* and the RAM are 64 bits wide (UM sec.16.2.1), so at a USP of 4 mod 8 every DR dpushm.d and */
/* dpopm.d move straddles two beats. The caller's frame sizes alone decide that residue, so   */
/* R0 is aligned down to 8 across the MVTC. The return address is popped FIRST: the aligned   */
/* R0 is then no lower than the slot the caller's bsr pushed, the 4 bytes                     */
/* trap_redzone_roots.txt charges this body.                                                  */
    .global _arch_irq_restore
_arch_irq_restore:
    mvfc    psw, r5
    and     #0xf0ffffff, r5
    shll    #24, r1
    and     #0x0f000000, r1
    or      r1, r5
    pop     r4                 /* return address; R0 = the caller's SP */
    mov.l   r0, r3
    and     #-KICKOS_RX_TRAP_BANK_ALIGN, r0
    mvtc    r5, psw            /* a pended SWINT is accepted here, on an 8-aligned USP */
    mov.l   r3, r0
    jmp     r4

/* THE INTB ENTRIES. INT #n and BRK are unprivileged and vector through INTB exactly as an
   acceptance does, so every slot but the syscall's tells the two apart before its handler runs.
   An acceptance sets PSW.IPL to the source's level, which is strictly above the interrupted one
   (HW UM sec.15.2.4); a trap leaves IPL alone (ISA UM sec.5.5.9). The interrupted IPL is the one
   in the PSW the hardware pushed on the ISP. */
    .macro INTB_GUARD entry, handler
    .global _\entry
_\entry:
    push.l  r15
    mvfc    psw, r15
    xor     8[r0].l, r15           /* [r0]=R15 4[r0]=PC 8[r0]=PSW */
    and     #IPL_FIELD, r15
    pop     r15                    /* POP leaves the flags */
    beq     1f
    bra.a   _\handler
1:
    bra.a   _kickos_rx_soft_trap
    .endm

    INTB_GUARD kickos_rx_intb_default, kickos_rx_default_irq
    INTB_GUARD kickos_rx_intb_swint2, kickos_rx_swint2
    INTB_GUARD kickos_rx_intb_timer, kickos_rx_timer_isr
    INTB_GUARD kickos_rx_intb_clock, kickos_rx_clock_isr
    INTB_GUARD kickos_rx_intb_sci6_rxi, kickos_rx_sci6_rxi_isr
    INTB_GUARD kickos_rx_intb_console_txi, kickos_rx_console_txi_isr

/* A software trap through an INTB slot, with [PC][PSW] at R0 on the ISP. Returns only when the
   trapping thread was redirected to its exit, which the RTE then lands in. */
    .global _kickos_rx_soft_trap
_kickos_rx_soft_trap:
    mov.l   r0, r1
    bsr.a   _kickos_rx_soft_trap_report
    rte

/* kickos_rx_pendsw: SWINT handler (INTB[27]). HW pushed PC then PSW to the ISP  */
/* and cleared U (so R0 = ISP). I stays 0 for the whole handler, so the swap is  */
/* atomic and g_arch_next is stable under it.                                    */
    .global _kickos_rx_pendsw
_kickos_rx_pendsw:
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
    /* INTB_GUARD's test, inline because this entry banks R15 anyway. */
    mvfc    psw, r15
    xor     8[r0].l, r15
    and     #IPL_FIELD, r15
    bne     .Lpendsw_accepted
    pop     r15
    bra.a   _kickos_rx_soft_trap
.Lpendsw_accepted:
    /* Validate the outgoing thread's live USP before the save builds a frame through it
       (rx_trap_stack.h). R14 is still the outgoing thread's, so bank it and use R15,
       already saved, as scratch. */
    push.l  r14
    mvfc    usp, r15
    mov.l   r15, r14
    and     #KICKOS_RX_TRAP_SP_ALIGN - 1, r14
    bne     .Lpendsw_bad_usp       /* misaligned USP: every store in the frame would be too */
    /* THE USP IS EITHER STACK. A thread preempted mid-dispatch is running on its own kernel
       block (svc_trampoline), so its USP is inside [kernel_sp - KICKOS_KERNEL_STACK_SIZE,
       kernel_sp) and the user-stack bounds below would refuse every blocking syscall and
       kill the thread. The block leg is narrowest on purpose: THIS thread's kernel_sp and no
       other, never the whole array. An unseated block reads 0, so the range test falls
       through to the user-stack legs, which is what idle takes.

       THE BLOCK LEG TAKES THE PRIVILEGE AND THE RANGE, AND NEITHER ALONE. PM alone accepts
       any supervisor USP, and a kernel thread running its own code has PM clear too. The
       RANGE alone accepts a USP an UNPRIVILEGED thread aimed at its own block, making the
       save below a supervisor write into kernel .bss at that thread's say-so. No MPU speaks
       to that write either: the RX MPU checks user mode only (UM sec.17.1.1) and every store
       in the save runs supervisor, so an enforcing MPU refuses the THREAD's own access to
       the block and permits the SWITCHER's frame through the pointer that thread supplied.

       RX acceptance clears PSW.U and pushes PC then PSW to the ISP whatever stack was
       interrupted (ISA UM sec.5.5.9), so the hardware neither writes through the USP nor
       validates it. The stacked PSW is the only record of the interrupted privilege, an
       exception setting PM to 0 and saving the pre-exception value (ISA UM sec.1.4.4).

       ROOM BELOW IS PART OF THE RANGE, the save being built BELOW the accepted USP. A bare
       range test admits the block's base, and KICKOS_RX_TRAP_FRAME_PENDSW bytes then land under
       the block with the canary above them untouched, so nothing reports it. The compare therefore stops
       BLOCK_USP_MAX_DROP short of the base. */
    mov.l   12[r0], r14            /* the interrupted PSW: [r0]=R14, 4=R15, 8=PC, 12=PSW */
    and     #MASK_PM, r14
    bne     .Lpendsw_user_usp      /* PM=1: user code was interrupted, so not on a block */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_KERNEL_SP[r14], r14
    cmp     r15, r14               /* kernel_sp - USP */
    bleu    .Lpendsw_user_usp      /* at or below the USP, so not on the block */
    sub     r15, r14               /* bytes from the USP up to the block's top */
    cmp     #BLOCK_USP_MAX_DROP, r14
    bleu    .Lpendsw_usp_ok        /* on this thread's block, a whole zone still below */
.Lpendsw_user_usp:
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_HI[r14], r14
    cmp     r14, r15               /* USP - stack_hi */
    bgtu    .Lpendsw_bad_usp
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_LO[r14], r14
    cmp     r14, r15               /* USP - stack_lo */
    bltu    .Lpendsw_bad_usp
    /* Room-remaining below the USP (rx_trap_stack.h carries the derivation). Taken only
       after the test above proved USP >= stack_lo, so a low USP cannot pass by wrapping
       the subtraction. */
    sub     r14, r15               /* r15 = writable bytes below the USP */
    cmp     #KICKOS_RX_TRAP_REDZONE_PENDSW, r15
    bltu    .Lpendsw_bad_usp       /* frame + kernel descent would run off the bottom */
.Lpendsw_usp_ok:
    /* A thread switched out inside an image's _kickos_user_cas restartable window resumes at
       its load, or its store lands over whatever the next thread wrote. The bound is INCLUSIVE
       of the store: a stacked PC on it means the store has not run. [r0] holds R14 and 4[r0]
       R15, so the interrupted PC is 8[r0] here. */
    UCAS_REWIND
.Lpendsw_ucas_out:
    pop     r14                    /* restore the outgoing thread's R14 */
    /* A software interrupt's ICU.IR027 survives acceptance: without this write back-to-back
       switches re-trigger a stale SWINT and the switcher live-locks. */
    mov.l   #ICU_IR_SWINT, r15
    mov.b   #0, [r15]
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
    mov.l   4[r0], F_CTX_RESUME_PC[r15]
    mov.l   8[r0], F_CTX_RESUME_PSW[r15]
    pop     r15                /* the outgoing thread's R15 */
    add     #8, r0             /* drop PC and PSW off the ISP */
    setpsw  u                  /* switch to the USP (R0 = USP) */
    pushm   r1-r15
    /* FPSW and the two accumulators, which a preempted thread may hold live. A1 is pushed
       before A0, so A0LO lands lowest and matches the frame layout above. */
    mvfc    fpsw, r15
    push.l  r15
    mvfacgu #0, a1, r15
    push.l  r15
    mvfachi #0, a1, r15
    push.l  r15
    mvfaclo #0, a1, r15
    push.l  r15
    mvfacgu #0, a0, r15
    push.l  r15
    mvfachi #0, a0, r15
    push.l  r15
    mvfaclo #0, a0, r15
    push.l  r15
#if defined(__RX_DFPU_INSNS__)
    /* DPFPU register file, banked below the accumulators: DR0-DR15 first (higher), then
       DPSW/DCMR/DECNT. arch_context_init fabricates the identical layout. */
    dpushm.d dr0-dr15
    dpushm.l dpsw-decnt
#endif
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15         /* outgoing ctx* (always valid: arch_start seeded it) */
    mov.l   r0, [r15]          /* outgoing->sp = R0 */
#if KICKOS_TELEMETRY
    mov.l   4[r15], r4         /* r4 = from_tid (outgoing.trace_tid, ctx offset 4) */
#endif
    mov.l   #_g_arch_next, r14
    mov.l   [r14], r15         /* incoming ctx* */
    mov.l   #_g_arch_current, r14
    mov.l   r15, [r14]         /* g_arch_current = incoming */
    clrpsw  u                  /* R0 = ISP */
#if KICKOS_TELEMETRY
    /* Emitted from the two PHYSICALLY-swapped contexts, and clobbers only registers the
       restore reloads. */
    mov.l   4[r15], r2         /* r2 = to_tid (incoming.trace_tid) */
    mov.l   r4, r1             /* r1 = from_tid */
    bsr.a   _kickos_trace_switch_done
#endif
    /* Shared restore epilogue: g_arch_current is the incoming context, PSW.I is 0 and every
       entry has dropped its trap words off the ISP. Every C call here runs on the ISP, never
       below the incoming frame on a stack its thread or a sibling can write. */
    .global _kickos_rx_restore
_kickos_rx_restore:
    clrpsw  u
    bsr.a   _kickos_arch_mpu_commit
    mov.l   #_g_arch_current, r1
    mov.l   [r1], r1
    bsr.a   _kickos_switch_landed
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
    push.l  F_CTX_RESUME_PSW[r15] /* RTE pops PC then PSW */
    push.l  F_CTX_RESUME_PC[r15]
    setpsw  u
    mov.l   [r15], r0          /* R0 = incoming->sp (frame base for the pops) */
#if defined(__RX_DFPU_INSNS__)
    /* The DPFPU file is the lowest part of the frame, so it reloads first. */
    dpopm.l  dpsw-decnt
    dpopm.d  dr0-dr15
#endif
    pop     r15
    mvtaclo r15, a0
    pop     r15
    mvtachi r15, a0
    pop     r15
    mvtacgu r15, a0
    pop     r15
    mvtaclo r15, a1
    pop     r15
    mvtachi r15, a1
    pop     r15
    mvtacgu r15, a1
    pop     r15
    mvtc    r15, fpsw
    popm    r1-r15             /* R1..R15 (R0/SP is not in the frame) */
    clrpsw  u                  /* R0 = ISP; the USP keeps the frame's top */
    rte

/* The guard refused the outgoing thread's USP. R14 is banked on the ISP above, so drop it,
   then hand the USP to the helper, which RETURNS ONLY WHEN THE THREAD WAS CONTAINED: R1 is
   then the context to run instead and the refused USP is touched by nothing below. The
   outgoing thread's R14 comes back off the ISP for the depth alone; the call clobbers it, and
   every register of a contained thread is discarded. */
.Lpendsw_bad_usp:
    mvfc    usp, r1
    pop     r14
    bsr.a   _kickos_rx_bad_usp
    /* A software interrupt's ICU.IR027 survives acceptance, exactly as on the path this one
       replaces: the switch it asked for is the one being made here. */
    mov.l   #ICU_IR_SWINT, r15
    mov.b   #0, [r15]
    mov.l   #_g_arch_current, r14
#if KICKOS_TELEMETRY
    /* The swap is physical, so it emits: kicktrace.py's verify() requires each SWITCH `from`
       to equal the previous `to`, and a silent swap breaks the NEXT record. R5 is the
       outgoing thread's and discarded with the rest, so it carries from_tid across. */
    mov.l   [r14], r15         /* outgoing ctx* = the offender */
    mov.l   4[r15], r5         /* from_tid (ctx offset 4) */
#endif
    mov.l   r1, [r14]          /* g_arch_current = incoming; NO save, the frame the guard
                                  refused to build is the one that would have carried it */
    mov.l   #_g_arch_next, r14
    mov.l   r1, [r14]          /* a SWINT nobody booked is then a self-switch */
    add     #12, r0            /* drop [R15][PC][PSW] off the ISP, uncopied */
#if KICKOS_TELEMETRY
    mov.l   4[r1], r2          /* to_tid */
    mov.l   r5, r1             /* from_tid */
    bsr.a   _kickos_trace_switch_done
#endif
    bra.a   _kickos_rx_restore

/* arch_start(boot, first): R1=boot (ignored; the ISP/boot stack is abandoned),  */
/* R2=first. The fabricated frame's PSW (IPL=0) releases the crit level          */
/* sched::start() held. Never returns.                                           */
    .global _arch_start
_arch_start:
    mov.l   #_g_arch_current, r3
    mov.l   r2, [r3]           /* g_arch_current = first */
    mov.l   #_g_arch_next, r3
    mov.l   r2, [r3]
#if KICKOS_TELEMETRY
    /* First-switch anchor: from=0xFFFF, which the decoder's verify() requires. On the ISP. */
    mov.l   4[r2], r2          /* r2 = to_tid (first.trace_tid, ctx offset 4) */
    mov.l   #0xFFFF, r1        /* r1 = from_tid = no-thread */
    bsr.a   _kickos_trace_switch_done
#endif
    bra.a   _kickos_rx_restore

/* kickos_rx_syscall_trap: installed at INTB[SYSCALL_VECTOR]. Entered on the ISP  */
/* with [R0]=userPC, [R0+4]=userPSW; R1-R4 hold the syscall args, INT saving no   */
/* GPRs. Scratch: R14, R15.                                                       */
    .global _kickos_rx_syscall_trap
_kickos_rx_syscall_trap:
    /* Validate the live USP before any path uses it (rx_trap_stack.h): the generic arm reads
       a3 above it, the fastpath arm writes the whole context save below it. R14/R15 are
       caller-saved at the syscall boundary, so they are free scratch. */
    mvfc    usp, r15
    mov.l   r15, r14
    and     #KICKOS_RX_TRAP_SP_ALIGN - 1, r14
    bne     .Lsys_bad_usp           /* misaligned USP: every store in the frame would be too */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_HI[r14], r14
    /* The a3 slot the generic arm reads sits ABOVE this USP (rx_trap_stack.h), so the bound is
       stack_hi less that slot: with the plain bound a USP AT the top passes and the read runs
       past it. stack_hi is a RAM address, so the subtraction cannot wrap. */
    sub     #KICKOS_RX_TRAP_HEAD_ABOVE_SYS, r14
    cmp     r14, r15                /* USP - (stack_hi - the a3 slot) */
    bgtu    .Lsys_bad_usp
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_LO[r14], r14
    cmp     r14, r15                /* USP - stack_lo */
    bltu    .Lsys_bad_usp
    /* Room-remaining below the USP, one figure covering both arms below
       (rx_trap_stack.h). Taken after the USP >= stack_lo test, so a low USP cannot pass
       by wrapping the subtraction. */
    sub     r14, r15                /* r15 = writable bytes below the USP */
    cmp     #KICKOS_RX_TRAP_REDZONE_SYS, r15
    bltu    .Lsys_bad_usp           /* the arms' own extents would run off the bottom */
    /* THE SEAM. KOS_SYS_CALL_REG completes here, before the RTE that would otherwise land in
       svc_trampoline and run the generic dispatch. INT stacks only PC and PSW, on the ISP,
       and leaves every GPR live, so this arm BUILDS the switch frame on the caller's own USP
       and hands the leaf the R1 slot inside it, which is what makes the reply land in the
       registers the restore pops. A refusal puts the frame back with nothing committed.

       THE FRAME STAYS ON THE CALLER'S USP, as a pendsw save does: it is the caller's saved
       context and ctx.sp names it. Its PC and PSW stay on the ISP until the taken arm copies
       them into the caller's context, the refusal below needing them there. What stays on the
       thread's own stack is a SAVE and not a C descent, and the leaf below runs on the
       block.

       One return address serves both outcomes: this frame carries the same stacked PC
       svc_trampoline would have returned to. */
    cmp     #56, r1            /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     .Lsys_generic
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
    /* Two trap-level preconditions the leaf cannot see. INT clears PSW.I, so an entry still
       carrying it is not a state the swap below is safe in; and a pended SWINT means
       kernel().current has already moved off the thread whose registers are live here. */
    mvfc    psw, r15
    and     #BIT_I, r15
    bne     .Lfp_isp_refused
    mov.l   #ICU_IR_SWINT, r15
    movu.b  [r15], r15
    cmp     #0, r15
    bne     .Lfp_isp_refused
    /* The pendsw save, so the frame the caller parks on is the one _kickos_rx_restore
       resumes. The accumulators are banked BEFORE the call: the leaf is C and may write
       them. */
    mvfc    usp, r15
    sub     #4, r15
    mov.l   [r0], [r15]        /* saved R15 */
    mvtc    r15, usp
    setpsw  u                  /* R0 = USP */
    pushm   r1-r14
    mvfc    fpsw, r15
    push.l  r15
    mvfacgu #0, a1, r15
    push.l  r15
    mvfachi #0, a1, r15
    push.l  r15
    mvfaclo #0, a1, r15
    push.l  r15
    mvfacgu #0, a0, r15
    push.l  r15
    mvfachi #0, a0, r15
    push.l  r15
    mvfaclo #0, a0, r15
    push.l  r15
#if defined(__RX_DFPU_INSNS__)
    dpushm.d dr0-dr15
    dpushm.l dpsw-decnt
#endif
    /* args = the R1 slot. Computed BEFORE the transfer below, so the leaf's array stays the
       frame's R1 slot whichever stack R0 is on. */
    mov.l   r0, r1
    add     #FRAME_R1_OFF, r1
    /* THE TRANSFER, for the leaf alone. The frame above stays on the USP (the head of this
       arm says why), but kickos_ipc_fastpath is a C dispatch and runs on the calling
       thread's own kernel block, so no C frame descends on a pointer the thread chose.

       PSW.I IS 0 ACROSS EVERY INSTRUCTION FROM HERE TO THE BRANCH OUT OF THIS ARM. INT
       cleared it, the precondition above REFUSED an entry that still carried it, and nothing
       here sets it: setpsw/clrpsw touch U alone and arch_irq_save/restore move PSW.IPL only.
       So no interrupt is accepted while R0 is off the frame, and the switcher cannot see
       this thread on its block with a half-built continuation. */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_KERNEL_SP[r14], r14
    cmp     #0, r14
    beq     .Lfp_usp_refused   /* no block: decline before the leaf runs, frame intact */
    sub     #4, r14
    mov.l   r0, [r14]          /* [T-4] = the frame base on this thread's own USP */
    mov.l   r14, r0            /* on the calling thread's kernel block */
    bsr.a   _kickos_ipc_fastpath
    /* Back on the frame BEFORE the verdict is read, so both outcomes see the R0 every
       instruction after this point expects. The cmp is what sets the flags, so the reload
       cannot disturb the branch. */
    mov.l   [r0], r0
    cmp     #0, r1
    beq     .Lfp_usp_refused
    /* Taken. R1 = the incoming thread's arch_context*, R0 = the outgoing frame base. */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15         /* outgoing ctx* (the caller) */
    mov.l   r0, [r15]          /* outgoing->sp = the frame just built */
    clrpsw  u                  /* R0 = ISP: [R15][PC][PSW] */
    mov.l   4[r0], F_CTX_RESUME_PC[r15]
    mov.l   8[r0], F_CTX_RESUME_PSW[r15]
    /* This path never RTEs out of the trap, so leaving the trap's three ISP words leaks a
       frame per call. */
    add     #12, r0
    mov.l   r1, [r14]          /* g_arch_current = incoming */
    mov.l   #_g_arch_next, r14
    mov.l   r1, [r14]
    bra.a   _kickos_rx_restore
/* The leaf declined, or there was no block to run it on. Nothing was committed: unwind to the
   register file and the USP the trap arrived with, so the generic path below runs on that
   exact state. R0 is the frame base on both entries to this label. Nothing on the block is
   live past the reload above, and PSW.I was 0 throughout, so the generic arm below is free to
   lay its own header on the block. */
.Lfp_usp_refused:
#if defined(__RX_DFPU_INSNS__)
    dpopm.l dpsw-decnt
    dpopm.d dr0-dr15
#endif
    pop     r15
    mvtaclo r15, a0
    pop     r15
    mvtachi r15, a0
    pop     r15
    mvtacgu r15, a0
    pop     r15
    mvtaclo r15, a1
    pop     r15
    mvtachi r15, a1
    pop     r15
    mvtacgu r15, a1
    pop     r15
    mvtc    r15, fpsw
    popm    r1-r15             /* R0 = the entry USP */
    clrpsw  u                  /* R0 = ISP */
.Lfp_isp_refused:
    pop     r15
/* THE TRANSFER. The return state goes into the continuation header on the calling thread's
   kernel block, copied from the ISP with PSW.I still 0, and the USP is pointed below it before
   the RTE, so svc_trampoline starts on the block and nothing is written through the USP the
   thread chose. */
.Lsys_generic:
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_KERNEL_SP[r14], r14
    cmp     #0, r14
    beq     .Lsys_nokstack
    sub     #SYSK_HEADER, r14
    mvfc    usp, r15
    mov.l   r15, 12[r14]       /* the resume USP */
    mov.l   4[r15], [r14]      /* a3, inside the slot the guard kept below stack_hi */
    mov.l   [r0], 4[r14]       /* userPC */
    mov.l   4[r0], 8[r14]      /* userPSW */
    mvtc    r14, usp
    mov.l   #_svc_trampoline, r15
    mov.l   r15, [r0]          /* stacked PC := svc_trampoline */
    mov.l   4[r0], r15
    and     #MASK_NOT_PM, r15  /* clear PM -> supervisor */
    or      #BIT_U, r15        /* set U -> the USP, now the block */
    mov.l   r15, 4[r0]
    rte

/* A thread whose ctx.kernel_sp is 0 reached the syscall path, so the transfer has nowhere to
   build. thread_create seats a block on every pool thread and idle runs arch_idle_wait alone,
   so this refuses rather than storing through 0. On the ISP with PSW.I clear, as the trap
   entered. */
.Lsys_nokstack:
    mvfc    usp, r1
    bsr.a   _kickos_rx_no_kernel_stack

/* The guard refused the calling thread's USP. Nothing was written through it and the ISP
   still carries [PC][PSW], the fastpath's own push being below the guard. Returns only when
   the thread was contained, R1 naming the context to run instead.

   NO ICU.IR027 WRITE HERE, unlike the pendsw twin: this is not the SWINT handler, and a
   switch pended behind this trap costs the resumed thread one self-switch through
   kickos_rx_pendsw rather than a live-lock. */
.Lsys_bad_usp:
    mvfc    usp, r1
    bsr.a   _kickos_rx_bad_usp
    mov.l   #_g_arch_current, r14
#if KICKOS_TELEMETRY
    /* The swap is physical, so it emits: kicktrace.py's verify() requires each SWITCH `from`
       to equal the previous `to`, and a silent swap breaks the NEXT record. R5 is the
       outgoing thread's and discarded with the rest, so it carries from_tid across. */
    mov.l   [r14], r15         /* outgoing ctx* = the offender */
    mov.l   4[r15], r5         /* from_tid (ctx offset 4) */
#endif
    mov.l   r1, [r14]          /* g_arch_current = incoming */
    mov.l   #_g_arch_next, r14
    mov.l   r1, [r14]
    add     #8, r0             /* drop [userPC][userPSW] off the ISP */
#if KICKOS_TELEMETRY
    mov.l   4[r1], r2          /* to_tid */
    mov.l   r5, r1             /* from_tid */
    bsr.a   _kickos_trace_switch_done
#endif
    bra.a   _kickos_rx_restore

/* svc_trampoline: entered by the trap's RTE, supervisor (PM=0) with R0 the USP the trap  */
/* pointed at the continuation header on the calling thread's own kernel block, and R1-R4 */
/* = nr,a0,a1,a2. The call balances R0. The way out stages [userPC][userPSW] on the ISP,  */
/* clearing PSW.I before PSW.U so no interrupt is accepted while R0 is the ISP.           */
    .global _svc_trampoline
_svc_trampoline:
    bsr.a   _syscall_dispatch  /* (R1-R4, [SP+4]=a3) -> result in R1:R2 */
    clrpsw  i
    mov.l   4[r0], r14         /* userPC */
    mov.l   8[r0], r15         /* userPSW */
    mov.l   12[r0], r5         /* the resume USP */
    clrpsw  u                  /* R0 = ISP */
    push.l  r15
    push.l  r14
    mvtc    r5, usp
    rte                        /* -> back in arch_syscall (R1:R2 = result) */

/* kickos_panic_stack_enter(msg, file, line, top): leave the interrupted stack for the
   reporter's own (arch.h). R1-R3 pass through untouched; R4 is the seat.
   PSW.I before the move. `setpsw u` before the write to R0 because R0 names the USP while
   PSW.U is set and the ISP once it is clear, and an exception is ACCEPTED on the ISP: putting
   the reporter on the USP is what leaves a nested fault a stack of its own instead of pushing
   it onto the reporter's. PSW.PM is untouched, so the reporter stays supervisor and the RX MPU,
   which checks user mode only, still does not read it. `bra` and not `bsr`: the
   -fcallgraph-info walk check_trap_redzone.sh runs stops at a body with no call edge, which is
   what takes the console tail out of every red zone. */
#if KICKOS_KERNEL_CORES > 1
#error "one kernel over several rxv3 cores has no per-core panic seat here"
#endif
    .global _kickos_panic_stack_enter
_kickos_panic_stack_enter:
    clrpsw  i
    setpsw  u
    mov.l   r4, r0
    bra.a   _kickos_panic_report
