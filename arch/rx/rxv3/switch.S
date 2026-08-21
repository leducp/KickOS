/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RXv3 context switch + syscall trap assembly (Renesas RX72M).
 *   1. arch_switch: record the target + pend the SWINT switcher. The physical swap
 *                   always happens in the SWINT handler, so ONE saved-frame format
 *                   serves both a voluntary block (thread context) and a preemptive
 *                   wake (ISR context).
 *   2. kickos_rx_pendsw: the deferred switcher, installed at INTB[SWINT]. Saves the
 *                   FULL interrupted context (R1-R15, FPSW, the two accumulators, PC,
 *                   PSW) on the outgoing thread's USP and restores the incoming one.
 *                   SWINT carries the LOWEST interrupt priority, so it is accepted
 *                   only after every other ISR drains. Runs with I=0, so g_arch_next
 *                   is stable under it.
 *   3. arch_start: enter the first thread through the shared restore epilogue.
 *   4. kickos_rx_syscall_trap + svc_trampoline: INT #SYS vectors to the trap on the
 *                   ISP; it rewrites the stacked PSW/PC so RTE returns to
 *                   svc_trampoline running SUPERVISOR-ON-USP (PM=0, U=1), i.e.
 *                   privileged execution on the calling thread's OWN stack, which is
 *                   what makes a blocking dispatch's continuation per-thread (arch.h
 *                   arch_syscall).
 *   5. arch_syscall: the userspace trap stub.
 *
 * psABI: args in R1-R4, 5th+ on the stack ([SP+4] at entry, [SP]=return address),
 * result in R1, callee-saved R6-R13 (GCC RX preserves R6-R13, wider than the
 * published R6-R11), R0=SP. INT saves ONLY PC then PSW to the ISP (ISA UM sec.5.5.9):
 * after INT [ISP]=PC, [ISP+4]=PSW; RTE pops PC then PSW. Interrupt acceptance switches
 * to the ISP (U=0) and pushes the frame there regardless of the interrupted stack
 * (RXv3 ISA / HW UM exception handling).
 *
 * Full saved frame, low->high on the thread's USP (ctx.sp points at the lowest
 * word = A0 low). Matches arch_context_init in arch_rxv3.cc:
 *     [+0 A0LO][+4 A0HI][+8 A0GU][+12 A1LO][+16 A1HI][+20 A1GU][+24 FPSW]
 *     [+28 R1 .. +84 R15][+88 PC][+92 PSW]
 * The word ABOVE the frame ([+96]) is the address a returning entry() RTSes into
 * (kickos_thread_return, or kickos_user_thread_return for a user thread).
 *
 * RX disp[Rn] addressing takes an UNSIGNED displacement only: every stack edit here
 * adjusts the pointer first, never uses a negative displacement.
 */

#include <kickos/arch/rx_trap_stack.h>

    .text

/* PSW bit masks (ISA UM sec.1.2.2.4): I=b16, U=b17, PM=b20. RX gas lexes a leading
 * "PSW"/"USP"/... as a control register, so these names avoid that. */
    .equ BIT_U,       0x00020000   /* PSW.U (select USP) */
    .equ MASK_NOT_PM, 0xffefffff   /* clear PSW.PM (-> supervisor) */
    .equ ICU_SWINTR,  0x000872E0   /* ICU.SWINTR: bit0 pends SWINT (vector 27) */
    .equ ICU_IR_SWINT, 0x0008701B  /* ICU.IR027: the SWINT request flag (0x87000+27) */
    .equ BIT_I,       0x00010000   /* PSW.I (interrupt enable) */
/* struct arch_context field offsets read by the USP bounds checks. Taken from
   rx_trap_stack.h, which arch_rxv3.cc static_asserts offsetof against. */
    .equ F_CTX_STACK_LO, KICKOS_RX_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_RX_CTX_OFF_STACK_HI
#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* Byte offset of the R1 slot from the frame base: the leaf's args, and the slot
   arch_ctx_set_syscall_result writes. arch_rxv3.cc asserts the same value. */
#if defined(__RX_DFPU_INSNS__)
    .equ FRAME_R1_OFF, 168
/* Above the R1 slot sit R1 itself (4), R2-R14 (52) and the [R15][PC][PSW] head (12), so the
   frame the guard prices is exactly 68 bigger. RX gas takes `.if <expr>` only, so the
   equality test is written as a difference against zero. */
    .if FRAME_R1_OFF + 68 - KICKOS_RX_TRAP_FRAME_PENDSW
    .error "FRAME_R1_OFF disagrees with KICKOS_RX_TRAP_FRAME_PENDSW (rx_trap_stack.h)"
    .endif
#else
    .equ FRAME_R1_OFF, 28
#endif
#endif
#if KICKOS_BENCH
    .equ CMTW1_CMWCNT, 0x00094290  /* free-running 7.5 MHz counter (bench time base) */
#endif

/* ------------------------------------------------------------------------- */
/* arch_switch(from, to): R1=from (ignored; the switcher saves g_arch_current),  */
/* R2=to. Called with the kernel IrqLock held (IPL>=SWINT), so SWINT is masked    */
/* and fires only once the lock releases (thread context) or the ISR chain drains */
/* (ISR context).                                                                */
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
/* and cleared U (so R0 = ISP). I stays 0 for the whole handler, so the swap is  */
/* atomic and g_arch_next is stable under it.                                    */
/* ------------------------------------------------------------------------- */
    .global _kickos_rx_pendsw
_kickos_rx_pendsw:
    /* R15 is free here (reloaded by the mvfc below). */
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
    /* Validate the outgoing thread's live USP before the save builds a frame through it
       (rx_trap_stack.h). Runs on the ISP; R14 is still the outgoing thread's, so bank it
       and use R15, already saved, as scratch. */
    push.l  r14
    mvfc    usp, r15
    mov.l   r15, r14
    and     #KICKOS_RX_TRAP_SP_ALIGN - 1, r14
    bne     .Lpendsw_bad_usp       /* misaligned USP: every store in the frame would be too */
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
    pop     r14                    /* restore the outgoing thread's R14 */
    /* A software interrupt's ICU.IR027 survives acceptance, so consume it here: without
       this write back-to-back switches re-trigger a stale SWINT and the switcher
       live-locks. */
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
    /* FPSW + the two accumulators (A0/A1: low, high, guard), which a preempted thread may
       hold live. MVFAC/MVTAC accumulator-move encodings per the RXv3 ISA UM. A1 is pushed
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
    /* DPFPU register file (64-bit doubles, -mdfpu), banked below the accumulators:
       DR0-DR15 first (higher addresses), then DPSW/DCMR/DECNT (lower). arch_context_init
       fabricates the identical layout and _kickos_rx_restore pops it in reverse. */
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
    mov.l   [r15], r0          /* R0 = incoming->sp */
#if KICKOS_BENCH
    /* Bench: switch-body delta (entry stamp -> now). CMTW1 ticks scaled to ICLK cycles
       (ICLK 240 MHz = CMTW1 7.5 MHz x32). R0 = incoming sp = the bottom of the frame, so
       the bsr's own frame lands below it. */
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
    /* Deferred MPU commit (arch_rxv3.cc, docs/design-mpu-commit-deferred.md): program the
       incoming thread's regions now that the physical swap (g_arch_current = incoming,
       R0 = incoming->sp) is complete, and before the frame is popped and RTE drops to user
       mode. R0 (=USP) survives the call (RX psABI) and the C function pushes BELOW the
       incoming frame; every register in the frame is popped next, so the call's
       caller-saved clobbers are harmless. */
    bsr.a   _kickos_arch_mpu_commit
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
    mov.l   [r15], r0          /* R0 = incoming->sp (frame base for the pops) */
#if defined(__RX_DFPU_INSNS__)
    /* The DPFPU file is the lowest part of the frame, so it reloads first, reverse of
       the save: DPSW/DCMR/DECNT (DPOPM.L) then DR0-DR15 (DPOPM.D). */
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

/* The guard refused the outgoing thread's USP. R14 is banked on the ISP above, so drop it,
   then hand the USP to the panic helper (noreturn). */
.Lpendsw_bad_usp:
    mvfc    usp, r1
    pop     r14
    bsr.a   _kickos_rx_bad_usp

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first): R1=boot (ignored; the ISP/boot stack is abandoned), */
/* R2=first. Selects the USP and enters `first` through the shared restore. The  */
/* fabricated frame's PSW (IPL=0) releases the crit level sched::start() held.    */
/* Never returns.                                                                */
/* ------------------------------------------------------------------------- */
    .global _arch_start
_arch_start:
    mov.l   #_g_arch_current, r3
    mov.l   r2, [r3]           /* g_arch_current = first */
    setpsw  u                  /* run on the USP (boot ran on the ISP) */
    mov.l   [r2], r0           /* R0 = first->sp (full fabricated frame) */
#if KICKOS_TELEMETRY
    /* First-switch anchor: emit SWITCH from=0xFFFF to=first.trace_tid, which the
       decoder's verify() requires. Runs on the incoming USP (R0=SP=first->sp); reload
       SP after the call. */
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
/* GPRs, so they must be preserved). Rewrites the frame so RTE lands in           */
/* svc_trampoline SUPERVISOR-ON-USP, after stashing the original userPC/userPSW on */
/* the thread's USP for the trampoline's return. Scratch: R5, R14, R15.           */
/* ------------------------------------------------------------------------- */
    .global _kickos_rx_syscall_trap
_kickos_rx_syscall_trap:
    /* On the ISP (INT cleared U). Validate the live USP before any path writes a frame
       through it (rx_trap_stack.h). R14/R15 are caller-saved at the syscall boundary, so
       they are free scratch here. */
    mvfc    usp, r15
    mov.l   r15, r14
    and     #KICKOS_RX_TRAP_SP_ALIGN - 1, r14
    bne     .Lsys_bad_usp           /* misaligned USP: every store in the frame would be too */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_HI[r14], r14
    cmp     r14, r15                /* USP - stack_hi */
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
    bltu    .Lsys_bad_usp           /* frame + dispatch would run off the bottom */
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    /* THE SEAM. KOS_SYS_CALL_REG completes here, before the RTE that would otherwise
       land in svc_trampoline and run the generic dispatch.

       INT stacks only PC and PSW, on the ISP, and leaves every GPR live, so this arm
       BUILDS the switch frame on the caller's own USP and hands the leaf the R1 slot
       inside it, which is what makes the reply land in the registers the restore pops.
       A refusal puts the frame back and falls through with nothing committed.

       One return address serves both outcomes: the frame this builds carries the same
       stacked PC svc_trampoline would have returned to, so RTE lands after the INT
       either way. */
    cmp     #56, r1            /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     .Lsys_generic
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
    /* Two trap-level preconditions the leaf cannot see. INT clears PSW.I, so an entry
       still carrying it is not a state the swap below is safe in; and a pended SWINT
       means kernel().current has already moved off the thread whose registers are live
       here. */
    mvfc    psw, r15
    and     #BIT_I, r15
    bne     .Lfp_isp_refused
    mov.l   #ICU_IR_SWINT, r15
    movu.b  [r15], r15
    cmp     #0, r15
    bne     .Lfp_isp_refused
    /* The pendsw save, verbatim, so the frame the caller parks on is the one
       _kickos_rx_restore resumes: [R15][PC][PSW] on the USP, then R1-R14, then FPSW and
       the two accumulators, then the DPFPU bank. The accumulators are banked BEFORE the
       call: the leaf is C and may write them. */
    mvfc    usp, r15
    sub     #12, r15
    mov.l   [r0], [r15]        /* saved R15 */
    mov.l   4[r0], 4[r15]      /* PC */
    mov.l   8[r0], 8[r15]      /* PSW */
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
    /* args = the R1 slot: args[0] = R1 (the number in, the result out), args[1] = R2
       (endpoint cap), args[2] = R3 (packed lengths), args[3..7] = R4-R8. */
    mov.l   r0, r1
    add     #FRAME_R1_OFF, r1
    bsr.a   _kickos_ipc_fastpath
    cmp     #0, r1
    beq     .Lfp_usp_refused
    /* Taken. R1 = the incoming thread's arch_context*, R0 = the outgoing frame base. */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r15         /* outgoing ctx* (the caller) */
    mov.l   r0, [r15]          /* outgoing->sp = the frame just built */
    mov.l   r1, [r14]          /* g_arch_current = incoming */
    /* Drop the trap's own three words off the ISP. This path never RTEs out of the trap,
       so leaving them leaks an ISP frame per call. */
    clrpsw  u                  /* R0 = ISP */
    add     #12, r0
    setpsw  u                  /* R0 = USP again */
    mov.l   [r1], r0           /* R0 = incoming->sp */
    bra.a   _kickos_rx_restore
.Lfp_usp_refused:
    /* Nothing was committed: unwind to the register file and the USP the trap arrived
       with, so the generic path below runs on that exact state. */
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
    popm    r1-r15             /* R1-R14 from the block, R15 from the head */
    add     #8, r0             /* past the head's [PC][PSW]: R0 = the entry USP */
    clrpsw  u                  /* R0 = ISP */
.Lfp_isp_refused:
    pop     r15
.Lsys_generic:
#endif
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

/* The guard refused the calling thread's USP. Nothing was written through it and the ISP
   still carries [PC][PSW]. Hand the USP to the panic helper (noreturn). */
.Lsys_bad_usp:
    mvfc    usp, r1
    bsr.a   _kickos_rx_bad_usp

/* Runs privileged (PM=0) on the calling thread's USP. [R0]=userPC,[R0+4]=userPSW */
/* (stashed above), a3 at [R0+12]; R1-R4 = nr,a0,a1,a2. A blocking syscall_dispatch */
/* here blocks by an ordinary arch_switch (pend SWINT); the switcher freezes the   */
/* mid-dispatch continuation on THIS USP and resumes it inline when rescheduled.   */
    .global _svc_trampoline
_svc_trampoline:
    mov.l   12[r0], r5         /* r5 = a3 (5th arg) */
    sub     #4, r0
    mov.l   r5, [r0]           /* [SP] = a3 */
    bsr.a   _syscall_dispatch  /* (R1-R4, [SP]=a3) -> result in R1:R2 */
    add     #4, r0             /* drop 5th-arg slot -> [R0]=userPC,[R0+4]=userPSW */
    /* rte pops PC+PSW only, so the R1:R2 pair reaches the caller untouched. */
    rte                        /* PC=userPC, PSW=userPSW -> back in arch_syscall (R1:R2=result) */

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3): R1=nr R2=a0 R3=a1 R4=a2, [R0+4]=a3. INT      */
/* preserves all GPRs, so no marshaling is needed; the trampoline returns here    */
/* (post-INT) in the caller's mode with R1:R2=result. arch_syscall64 is the SAME  */
/* trap: R1:R2 is the RX ABI's 8-byte return pair.                                */
/* ------------------------------------------------------------------------- */
    .global _arch_syscall
    .global _arch_syscall64
_arch_syscall:
_arch_syscall64:
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    rts                        /* return to caller, R1:R2 = result */

#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* ------------------------------------------------------------------------- */
/* int32_t arch_syscall_reg(uint32_t* io): R1 = io. INT preserves every GPR, so   */
/* the payload rides R4-R8 straight into the trap and the trap's own frame build  */
/* is what stores it. R6-R8 are callee-saved and are banked here. One return      */
/* address serves both outcomes: RTE lands after the INT whether the trap took    */
/* the fastpath or svc_trampoline ran the generic dispatch, so R1 is the result   */
/* either way.                                                                    */
/* ------------------------------------------------------------------------- */
    .global _arch_syscall_reg
_arch_syscall_reg:
    pushm   r6-r8
    push.l  r1                 /* io, across the trap */
    mov.l   4[r1], r2          /* io[1] = endpoint cap */
    mov.l   8[r1], r3          /* io[2] = packed lengths */
    mov.l   12[r1], r4         /* io[3..7] = the request payload */
    mov.l   16[r1], r5
    mov.l   20[r1], r6
    mov.l   24[r1], r7
    mov.l   28[r1], r8
    mov.l   [r1], r1           /* nr, last: it overwrites the pointer */
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    /* R2-R6 carry the reply on the taken path and the untouched request on the declined
       one. The stub reads them only when the result is not KOS_CALL_REG_FALLBACK, so an
       unconditional store is observationally free on the declined path. */
    mov.l   [r0], r14          /* io */
    mov.l   r2, 4[r14]
    mov.l   r3, 8[r14]
    mov.l   r4, 12[r14]
    mov.l   r5, 16[r14]
    mov.l   r6, 20[r14]
    add     #4, r0
    popm    r6-r8
    rts
#endif
