/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RXv3 context switch + syscall trap assembly (Renesas RX72M). Mechanisms:
 *   1. arch_switch        -- record the target + pend the SWINT switcher. NEVER
 *                            swaps inline (the ARM PendSV model): the physical
 *                            swap always happens in the SWINT handler, so there
 *                            is ONE saved-frame format for both a voluntary block
 *                            (thread context) and a preemptive wake (ISR context).
 *   2. kickos_rx_pendsw   -- the deferred switcher, installed at INTB[SWINT]. It
 *                            saves the FULL interrupted context (R1-R15, FPSW, the
 *                            two accumulators, PC, PSW) on the outgoing thread's
 *                            USP and restores the incoming one. Give SWINT the
 *                            LOWEST interrupt priority so it is accepted only after
 *                            every other ISR drains -- the tail-chain PendSV relies
 *                            on. Runs with I=0 (atomic; no nesting, so g_arch_next
 *                            cannot be rewritten under it).
 *   3. arch_start         -- enter the first thread: no outgoing to save, so it
 *                            jumps straight into the shared restore epilogue.
 *   4. kickos_rx_syscall_trap + svc_trampoline -- the syscall trap. INT #SYS
 *                            vectors to the trap on the ISP; it rewrites the stacked
 *                            PSW/PC so RTE returns to svc_trampoline running
 *                            SUPERVISOR-ON-USP (PM=0, U=1) = privileged execution on
 *                            the calling thread's OWN stack, so a blocking dispatch's
 *                            continuation is per-thread (arch.h arch_syscall).
 *   5. arch_syscall       -- the userspace trap stub.
 *
 * psABI (verified by disassembling rx-elf-gcc -misa=v3): args in R1-R4, 5th+ on
 * the stack ([SP+4] at entry, [SP]=return address), result in R1; callee-saved =
 * R6-R13 (empirically GCC RX preserves R6-R13, NOT R6-R11); R0=SP. INT saves ONLY
 * PC then PSW to the ISP (ISA UM sec.5.5.9): after INT [ISP]=PC, [ISP+4]=PSW; RTE
 * pops PC then PSW. Interrupt acceptance switches to the ISP (U=0) and pushes the
 * frame there regardless of the interrupted stack (RXv3 ISA / HW UM exception-
 * handling; the full-context frame layout follows the RX psABI register split).
 *
 * Full saved frame, low->high on the thread's USP (ctx.sp points at the lowest
 * word = A0 low). Matches arch_context_init in arch_rxv3.cc:
 *     [+0 A0LO][+4 A0HI][+8 A0GU][+12 A1LO][+16 A1HI][+20 A1GU][+24 FPSW]
 *     [+28 R1 .. +84 R15][+88 PC][+92 PSW]
 * The word ABOVE the frame ([+96]) is the address a returning entry() RTSes into
 * (kickos_thread_return, or kickos_user_thread_return for a user thread).
 *
 * RX disp[Rn] addressing takes an UNSIGNED displacement only -- every stack edit
 * here adjusts the pointer first, never uses a negative displacement.
 */

    .text

/* PSW bit masks (ISA UM sec.1.2.2.4): I=b16, U=b17, PM=b20. NB: RX gas lexes a
 * leading "PSW"/"USP"/... as a control register, so these names avoid that. */
    .equ BIT_U,       0x00020000   /* PSW.U (select USP) */
    .equ MASK_NOT_PM, 0xffefffff   /* clear PSW.PM (-> supervisor) */
    .equ ICU_SWINTR,  0x000872E0   /* ICU.SWINTR: bit0 pends SWINT (vector 27) */
    .equ ICU_IR_SWINT, 0x0008701B  /* ICU.IR027: the SWINT request flag (0x87000+27) */
#if KICKOS_BENCH
    .equ CMTW1_CMWCNT, 0x00094290  /* free-running 7.5 MHz counter (bench time base) */
#endif

/* ------------------------------------------------------------------------- */
/* arch_switch(from, to): R1=from (ignored -- the switcher saves g_arch_current),*/
/* R2=to. Record the target and pend the SWINT switcher. Called with the kernel */
/* IrqLock held (IPL>=SWINT), so SWINT is masked and fires only once the lock    */
/* releases (thread context) or the ISR chain drains (ISR context) -- deferred,  */
/* exactly like the ARM PendSV backend.                                          */
/* ------------------------------------------------------------------------- */
    .global _arch_switch
_arch_switch:
    mov.l   #_g_arch_next, r3
    mov.l   r2, [r3]           /* g_arch_next = to */
    mov.l   #ICU_SWINTR, r3
    mov.b   #1, [r3]           /* pend SWINT (deferred switch) */
    rts

/* ------------------------------------------------------------------------- */
/* kickos_rx_pendsw: SWINT handler (INTB[27]). HW pushed PC then PSW to the ISP  */
/* and cleared U (so R0 = ISP). Save the outgoing thread's full context on its   */
/* USP, swap g_arch_current<-g_arch_next, and restore the incoming one. I stays  */
/* 0 for the whole handler (atomic switch; g_arch_next is stable under it).      */
/* ------------------------------------------------------------------------- */
    .global _kickos_rx_pendsw
_kickos_rx_pendsw:
    /* Consume the SWINT request in-handler (ICU.IR027 is NOT auto-cleared on
       acceptance for the software interrupt; every other RX ISR clears its own IR
       flag likewise). Without this, back-to-back switches re-trigger a stale SWINT
       and the switcher live-locks (~10M switches, no forward progress) on HW; the
       slow path masked it. R15 is free here (reloaded by the mvfc below). */
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
    mov.l   #ICU_IR_SWINT, r15
    mov.b   #0, [r15]
#if KICKOS_BENCH
    /* Bench: stamp switch entry (CMTW1). Balanced push/pop keeps the ISP frame. */
    pushm   r14-r15
    mov.l   #CMTW1_CMWCNT, r15
    mov.l   [r15], r14
    mov.l   #_g_bench_sw_start, r15
    mov.l   r14, [r15]
    popm    r14-r15
#endif
    mvfc    usp, r15
    sub     #12, r15
    mvtc    r15, usp           /* room for [R15][PC][PSW] on the USP */
    mov.l   [r0], [r15]        /* saved R15 */
    mov.l   4[r0], 4[r15]      /* PC */
    mov.l   8[r0], 8[r15]      /* PSW */
    add     #12, r0            /* drop the 3 words off the ISP */
    setpsw  u                  /* switch to the USP (R0 = USP) */
    pushm   r1-r14             /* the rest of the GPRs, below [R15][PC][PSW] */
    /* FPSW + the two accumulators (A0/A1: low, high, guard) -- a preempted thread
       may hold DSP/FP state, so save it. Registers + the MVFAC/MVTAC accumulator-
       move encodings per the RXv3 ISA UM; A0-then-A1 push order pairs the restore. */
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
    /* DPFPU register file (64-bit doubles, -mdfpu): a preempted thread may hold
       live DR state at an arbitrary PC, so bank it below the accumulators. DR0-DR15
       first (higher addresses), then DPSW/DCMR/DECNT (lower) -- arch_context_init
       fabricates the identical layout; _kickos_rx_restore pops it in reverse. */
    dpushm.d dr0-dr15
    dpushm.l dpsw-decnt
#endif
    /* R0 now = the outgoing thread's full frame. Store it into g_arch_current. */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15         /* outgoing ctx* (always valid: arch_start seeded it) */
    mov.l   r0, [r15]          /* outgoing->sp = R0 */
#if KICKOS_TELEMETRY
    mov.l   4[r15], r4         /* r4 = from_tid (outgoing.trace_tid, ctx offset 4) */
#endif
    /* g_arch_current = g_arch_next (the incoming thread). */
    mov.l   #_g_arch_next, r14
    mov.l   [r14], r15         /* incoming ctx* */
    mov.l   #_g_arch_current, r14
    mov.l   r15, [r14]         /* g_arch_current = incoming */
    mov.l   [r15], r0          /* R0 = incoming->sp */
#if KICKOS_BENCH
    /* Bench: switch-body delta (entry stamp -> now). CMTW1 ticks scaled to ICLK
       cycles (ICLK 240 MHz = CMTW1 7.5 MHz x32), via the shared accumulator. Measures
       the save+swap window; a C call is safe here (R0 = incoming sp = bottom of the
       frame, so bsr pushes below it -- same as the telemetry hook). */
    mov.l   #CMTW1_CMWCNT, r1
    mov.l   [r1], r1
    mov.l   #_g_bench_sw_start, r2
    mov.l   [r2], r2
    sub     r2, r1             /* r1 = delta (CMTW1 ticks) */
    shll    #5, r1             /* x32 -> ~ICLK cycles */
    bsr.a   _kickos_bench_switch_done
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15
    mov.l   [r15], r0          /* reload R0 = incoming->sp */
#endif
#if KICKOS_TELEMETRY
    /* SWITCH-completion hook, emitted from the two PHYSICALLY-swapped contexts.
       Runs on the incoming USP (below its saved frame) with I=0; clobbers only
       caller-saved regs, all of which the restore below reloads. */
    mov.l   4[r15], r2         /* r2 = to_tid (incoming.trace_tid) */
    mov.l   r4, r1             /* r1 = from_tid */
    bsr.a   _kickos_trace_switch_done
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15
    mov.l   [r15], r0          /* reload R0 = incoming->sp (SP survived the call) */
#endif
    /* Shared restore epilogue (also entered by arch_start). R0 = incoming frame. */
    .global _kickos_rx_restore
_kickos_rx_restore:
#if defined(__RX_DFPU_INSNS__)
    /* Reload the DPFPU file first (it is the lowest part of the frame) -- reverse
       of the save: DPSW/DCMR/DECNT (DPOPM.L) then DR0-DR15 (DPOPM.D). */
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
    rte                        /* pops PC then PSW -> resume incoming */

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first): R1=boot (ignored -- ISP/boot stack abandoned),      */
/* R2=first. No outgoing context to save; select the USP and enter first via the */
/* shared restore. The fabricated frame's PSW (IPL=0) releases the crit level    */
/* sched::start() held. Never returns.                                           */
/* ------------------------------------------------------------------------- */
    .global _arch_start
_arch_start:
    mov.l   #_g_arch_current, r3
    mov.l   r2, [r3]           /* g_arch_current = first */
    setpsw  u                  /* run on the USP (boot ran on the ISP) */
    mov.l   [r2], r0           /* R0 = first->sp (full fabricated frame) */
#if KICKOS_TELEMETRY
    /* First-switch anchor: emit SWITCH from=0xFFFF to=first.trace_tid, so the RX
       telemetry stream carries the first-switch record every other arch emits (the
       decoder's verify() requires from==0xFFFF). Runs on the incoming USP
       (R0=SP=first->sp) like the pendsw emit; reload SP after the call. */
    mov.l   4[r2], r2          /* r2 = to_tid (first.trace_tid, ctx offset 4) */
    mov.l   #0xFFFF, r1        /* r1 = from_tid = no-thread */
    bsr.a   _kickos_trace_switch_done
    mov.l   #_g_arch_current, r3
    mov.l   [r3], r2
    mov.l   [r2], r0           /* reload R0 = first->sp (SP for the restore) */
#endif
    bra.a   _kickos_rx_restore

/* ------------------------------------------------------------------------- */
/* kickos_rx_syscall_trap: installed at INTB[SYSCALL_VECTOR]. Entered on the ISP */
/* with [R0]=userPC, [R0+4]=userPSW; R1-R4 hold the syscall args (INT saves no    */
/* GPRs -- must preserve them). Rewrites the frame so RTE lands in svc_trampoline */
/* SUPERVISOR-ON-USP, after stashing the original userPC/userPSW on the thread's  */
/* USP for the trampoline's return. Scratch: R5, R14, R15 (caller-saved).         */
/* ------------------------------------------------------------------------- */
    .global _kickos_rx_syscall_trap
_kickos_rx_syscall_trap:
    mvfc    usp, r15
    sub     #8, r15            /* new USP top: room for [userPC][userPSW] */
    mov.l   [r0], r14          /* userPC */
    mov.l   r14, [r15]         /* [USP-8] = userPC */
    mov.l   4[r0], r14         /* userPSW */
    mov.l   r14, 4[r15]        /* [USP-4] = userPSW */
    mvtc    r15, usp           /* commit: [USP]=userPC,[USP+4]=userPSW,a3@[USP+12] */
    mov.l   #_svc_trampoline, r14
    mov.l   r14, [r0]          /* stacked PC := svc_trampoline */
    mov.l   4[r0], r14         /* userPSW */
    and     #MASK_NOT_PM, r14   /* clear PM -> supervisor (privileged) */
    or      #BIT_U, r14    /* set U -> the thread's own USP */
    mov.l   r14, 4[r0]         /* stacked PSW := supervisor-on-USP */
    rte                        /* -> svc_trampoline, PM=0 U=1, on the USP */

/* Runs privileged (PM=0) on the calling thread's USP. [R0]=userPC,[R0+4]=userPSW */
/* (stashed above), a3 at [R0+12]; R1-R4 = nr,a0,a1,a2. A blocking syscall_dispatch */
/* here blocks by an ordinary arch_switch (pend SWINT); the switcher freezes the   */
/* mid-dispatch continuation on THIS USP and resumes it inline when rescheduled.   */
    .global _svc_trampoline
_svc_trampoline:
    mov.l   12[r0], r5         /* r5 = a3 (5th arg) */
    sub     #4, r0
    mov.l   r5, [r0]           /* [SP] = a3 */
    bsr.a   _syscall_dispatch  /* (R1-R4, [SP]=a3) -> result in R1 */
    add     #4, r0             /* drop 5th-arg slot -> [R0]=userPC,[R0+4]=userPSW */
    rte                        /* PC=userPC, PSW=userPSW -> back in arch_syscall (R1=result) */

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3): R1=nr R2=a0 R3=a1 R4=a2, [R0+4]=a3. INT      */
/* preserves all GPRs, so no marshaling is needed; the trampoline returns here    */
/* (post-INT) in the caller's mode with R1=result.                                */
/* ------------------------------------------------------------------------- */
    .global _arch_syscall
_arch_syscall:
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    rts                        /* return to caller, R1 = result */
