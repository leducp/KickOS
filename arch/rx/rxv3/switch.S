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
 *                   svc_trampoline running SUPERVISOR-ON-USP (PM=0, U=1). The
 *                   trampoline's first act is to relocate R0 onto the CALLING THREAD'S
 *                   KERNEL BLOCK (ctx.kernel_sp) and dispatch there, so no privileged
 *                   store and no C frame reaches the pointer the thread chose, and a
 *                   blocking dispatch's continuation is still per-thread (arch.h
 *                   arch_syscall). rx_trap_stack.h derives what each stack holds.
 *   5. arch_syscall: the userspace trap stub.
 *
 * psABI: args in R1-R4, 5th+ on the stack ([SP+4] at entry, [SP]=return address), result in
 * R1, callee-saved R6-R13 (GCC RX preserves R6-R13, wider than the published R6-R11), R0=SP.
 * INT saves ONLY PC then PSW to the ISP (ISA UM sec.5.5.9), and RTE pops PC then PSW.
 * Interrupt acceptance switches to the ISP (U=0) and pushes the frame there regardless of
 * the interrupted stack.
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
/* KICKOS_KERNEL_STACK_SIZE, which kickos_rx_pendsw compares a USP against. Pure integer
   macros, generated. */
#include <kickos/board_config.h>

    .text

/* PSW bit masks (ISA UM sec.1.2.2.4): I=b16, U=b17, PM=b20. RX gas lexes a leading
 * "PSW"/"USP"/... as a control register, so these names avoid that. */
    .equ BIT_U,       0x00020000   /* PSW.U (select USP) */
    .equ MASK_NOT_PM, 0xffefffff   /* clear PSW.PM (-> supervisor) */
    .equ MASK_PM,     0x00100000   /* PSW.PM: 0 supervisor, 1 user */
    .equ ICU_SWINTR,  0x000872E0   /* ICU.SWINTR: bit0 pends SWINT (vector 27) */
    .equ ICU_IR_SWINT, 0x0008701B  /* ICU.IR027: the SWINT request flag (0x87000+27) */
    .equ BIT_I,       0x00010000   /* PSW.I (interrupt enable) */
/* struct arch_context field offsets read by the trap sites. Taken from rx_trap_stack.h,
   which arch_rxv3.cc static_asserts offsetof against. */
    .equ F_CTX_STACK_LO, KICKOS_RX_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_RX_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_RX_CTX_OFF_KERNEL_SP
/* The [userPC][userPSW] head the generic arm commits, and the fifth-argument slot above it.
   Both from rx_trap_stack.h so the head and the steps over it cannot drift. */
    .equ SYS_HEAD,    KICKOS_RX_TRAP_HEAD_SYS
    .equ SYS_A3_OFF,  KICKOS_RX_TRAP_HEAD_SYS + 4
/* The deepest a USP may sit inside a kernel block and still be accepted by
   kickos_rx_pendsw, as a distance DOWN from the block's top because that is what the compare
   there holds. SIZE, less the lowest word (the overflow canary in kernel/thread/thread.cc,
   which a frame must not consume), less the save the switcher builds below the accepted USP
   plus the restore descent under it (KICKOS_RX_TRAP_REDZONE_PENDSW). Positive by
   arch_rxv3.cc's KICKOS_RX_TRAP_NEED_SYSK assert, which already requires SIZE - 4 to hold
   1096 bytes. */
    .equ BLOCK_USP_MAX_DROP, \
        KICKOS_KERNEL_STACK_SIZE - KICKOS_RX_TRAP_REDZONE_PENDSW - 4
#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* Byte offset of the R1 slot from the frame base: the leaf's args, and the slot
   arch_ctx_set_syscall_result writes. arch_rxv3.cc asserts the same value. */
#if defined(__RX_DFPU_INSNS__)
    .equ FRAME_R1_OFF, 168
/* Above the R1 slot sit R1 itself (4), R2-R14 (52) and the [R15][PC][PSW] head (12), so the
   frame the guard prices is exactly 68 bigger. RX gas takes `.if <expr>` only, so the
   equality is written as a difference against zero. */
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

/* kickos_rx_pendsw: SWINT handler (INTB[27]). HW pushed PC then PSW to the ISP  */
/* and cleared U (so R0 = ISP). I stays 0 for the whole handler, so the swap is  */
/* atomic and g_arch_next is stable under it.                                    */
    .global _kickos_rx_pendsw
_kickos_rx_pendsw:
    /* R15 is free here (reloaded by the mvfc below). */
    push.l  r15                /* [ISP]=R15, 4[ISP]=PC, 8[ISP]=PSW */
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

       ARM needs neither test because its exception entry stacks the frame ABOVE the PSP at
       the PRE-exception privilege, so a kernel-aimed PSP is refused as MSTKERR before the
       handler runs. RX acceptance clears PSW.U and pushes PC then PSW to the ISP whatever
       stack was interrupted (ISA UM sec.5.5.9), so the hardware neither writes through the
       USP nor validates it. The stacked PSW is the only record of the interrupted privilege,
       an exception setting PM to 0 and saving the pre-exception value (ISA UM sec.1.4.4).

       ROOM BELOW IS PART OF THE RANGE, the save being built BELOW the accepted USP. A bare
       range test admits the block's base, and 236 bytes then land under the block with the
       canary above them untouched, so nothing reports it. The compare therefore stops
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
    pop     r14                    /* restore the outgoing thread's R14 */
    /* A software interrupt's ICU.IR027 survives acceptance: without this write back-to-back
       switches re-trigger a stale SWINT and the switcher live-locks. */
    mov.l   #ICU_IR_SWINT, r15
    mov.b   #0, [r15]
#if KICKOS_BENCH
    /* Balanced push/pop keeps the ISP frame. */
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
    mov.l   [r15], r0          /* R0 = incoming->sp */
#if KICKOS_BENCH
    /* CMTW1 ticks scaled to ICLK cycles (ICLK 240 MHz = CMTW1 7.5 MHz x32). R0 = incoming
       sp = the bottom of the frame, so the bsr's own frame lands below it. */
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
    /* Emitted from the two PHYSICALLY-swapped contexts. Runs on the incoming USP with I=0
       and clobbers only caller-saved regs, all of which the restore below reloads. */
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
    /* Deferred MPU commit, after the physical swap and
       before the frame is popped and RTE drops to user mode. R0 survives the call (RX psABI)
       and the C function pushes BELOW the incoming frame; every register in the frame is
       popped next, so the caller-saved clobbers are harmless. */
    bsr.a   _kickos_arch_mpu_commit
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
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
    rte                        /* pops PC then PSW -> resume incoming */

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
    add     #12, r0            /* drop [R15][PC][PSW] off the ISP, uncopied */
    setpsw  u                  /* R0 = USP */
    mov.l   [r1], r0           /* R0 = incoming->sp */
#if KICKOS_TELEMETRY
    mov.l   4[r1], r2          /* to_tid */
    mov.l   r5, r1             /* from_tid */
    bsr.a   _kickos_trace_switch_done
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
    mov.l   [r15], r0          /* reload R0 = incoming->sp (SP survived the call) */
#endif
    bra.a   _kickos_rx_restore

/* arch_start(boot, first): R1=boot (ignored; the ISP/boot stack is abandoned),  */
/* R2=first. The fabricated frame's PSW (IPL=0) releases the crit level          */
/* sched::start() held. Never returns.                                           */
    .global _arch_start
_arch_start:
    mov.l   #_g_arch_current, r3
    mov.l   r2, [r3]           /* g_arch_current = first */
    setpsw  u                  /* run on the USP (boot ran on the ISP) */
    mov.l   [r2], r0           /* R0 = first->sp (full fabricated frame) */
#if KICKOS_TELEMETRY
    /* First-switch anchor: from=0xFFFF, which the decoder's verify() requires. Runs on the
       incoming USP, so SP is reloaded after the call. */
    mov.l   4[r2], r2          /* r2 = to_tid (first.trace_tid, ctx offset 4) */
    mov.l   #0xFFFF, r1        /* r1 = from_tid = no-thread */
    bsr.a   _kickos_trace_switch_done
    mov.l   #_g_arch_current, r3
    mov.l   [r3], r2
    mov.l   [r2], r0           /* reload R0 = first->sp (SP for the restore) */
#endif
    bra.a   _kickos_rx_restore

/* kickos_rx_syscall_trap: installed at INTB[SYSCALL_VECTOR]. Entered on the ISP  */
/* with [R0]=userPC, [R0+4]=userPSW; R1-R4 hold the syscall args, INT saving no   */
/* GPRs. Rewrites the frame so RTE lands in svc_trampoline SUPERVISOR-ON-USP,     */
/* after stashing the original userPC/userPSW on the thread's USP for the         */
/* trampoline's return. Scratch: R5, R14, R15.                                    */
    .global _kickos_rx_syscall_trap
_kickos_rx_syscall_trap:
    /* Validate the live USP before any path writes through it (rx_trap_stack.h): the generic
       arm the [userPC][userPSW] head, the fastpath arm the whole context save. R14/R15 are
       caller-saved at the syscall boundary, so they are free scratch. */
    mvfc    usp, r15
    mov.l   r15, r14
    and     #KICKOS_RX_TRAP_SP_ALIGN - 1, r14
    bne     .Lsys_bad_usp           /* misaligned USP: every store in the frame would be too */
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_STACK_HI[r14], r14
    /* The a3 slot svc_trampoline reads sits ABOVE this USP (rx_trap_stack.h), so the bound is
       stack_hi less that slot: with the plain bound a USP AT the top passes and the
       trampoline reads past it. stack_hi is a RAM address, so the subtraction cannot wrap. */
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
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    /* THE SEAM. KOS_SYS_CALL_REG completes here, before the RTE that would otherwise land in
       svc_trampoline and run the generic dispatch. INT stacks only PC and PSW, on the ISP,
       and leaves every GPR live, so this arm BUILDS the switch frame on the caller's own USP
       and hands the leaf the R1 slot inside it, which is what makes the reply land in the
       registers the restore pops. A refusal puts the frame back with nothing committed.

       THE FRAME CANNOT MOVE TO THE KERNEL BLOCK: it is the caller's saved context, ctx.sp
       names it and _kickos_rx_restore RTEs off it. RTE pops PC then PSW from R0, and R0 IS
       the USP once the popped PSW sets U, so the caller resumes on frame_base + 236, which
       on a block would be an SP inside kernel .bss. What stays on the thread's own stack is
       therefore a SAVE and not a C descent, and the leaf below runs on the block.

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
    /* The pendsw save, verbatim, so the frame the caller parks on is the one
       _kickos_rx_restore resumes. The accumulators are banked BEFORE the call: the leaf is C
       and may write them. */
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
    mov.l   r1, [r14]          /* g_arch_current = incoming */
    /* This path never RTEs out of the trap, so leaving the trap's three ISP words leaks a
       frame per call. */
    clrpsw  u                  /* R0 = ISP */
    add     #12, r0
    setpsw  u                  /* R0 = USP again */
    mov.l   [r1], r0           /* R0 = incoming->sp */
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
    popm    r1-r15             /* R1-R14 from the block, R15 from the head */
    add     #SYS_HEAD, r0      /* past the head's [PC][PSW]: R0 = the entry USP */
    clrpsw  u                  /* R0 = ISP */
.Lfp_isp_refused:
    pop     r15
.Lsys_generic:
#endif
    mvfc    usp, r15
    sub     #SYS_HEAD, r15     /* new USP top: room for [userPC][userPSW] */
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
    add     #8, r0             /* drop [userPC][userPSW] off the ISP */
    setpsw  u                  /* R0 = USP */
    mov.l   [r1], r0           /* R0 = incoming->sp */
#if KICKOS_TELEMETRY
    mov.l   4[r1], r2          /* to_tid */
    mov.l   r5, r1             /* from_tid */
    bsr.a   _kickos_trace_switch_done
    mov.l   #_g_arch_current, r15
    mov.l   [r15], r15
    mov.l   [r15], r0          /* reload R0 = incoming->sp (SP survived the call) */
#endif
    bra.a   _kickos_rx_restore

/* THE TRANSFER. Entered privileged (PM=0) by the trap's own RTE with R0 = the calling   */
/* thread's USP: [R0]=userPC, [R0+4]=userPSW, a3 at [R0+12], R1-R4 = nr,a0,a1,a2, and    */
/* R5/R14/R15 free. It relocates R0 onto that thread's own kernel block and calls        */
/* syscall_dispatch there, so no privileged store and no C frame reaches the pointer     */
/* the thread chose. NOTHING IS PUSHED FIRST: three free scratch registers reach         */
/* ctx.kernel_sp, so the only bytes this arm spends on the thread's own stack are the    */
/* eight the trap above committed.                                                       */
/*                                                                                       */
/* THE CONTINUATION HEADER, laid at the block's top T and read back after the call:      */
/*     [T-8]  a3, the psABI fifth argument, which syscall_dispatch reads at [SP+4]       */
/*     [T-4]  the USP the thread resumes on                                             */
/* The resume USP lives HERE and not in a register or a second context field: a blocking */
/* dispatch has the SWINT switcher freeze its continuation BELOW this header, so the     */
/* header survives the block for free. The call balances R0.                             */
/*                                                                                       */
/* THE HEAD STAYS ON THE THREAD'S STACK because RTE pops PC then PSW from R0 and R0 IS   */
/* the USP while PSW.U is set, so the hardware computes the resume USP as the head's     */
/* address plus eight.                                                                    */
    .global _svc_trampoline
_svc_trampoline:
    mov.l   #_g_arch_current, r14
    mov.l   [r14], r14
    mov.l   F_CTX_KERNEL_SP[r14], r14
    cmp     #0, r14
    beq     .Lsvc_nokstack     /* no block to build on, and it does not return */
    mov.l   SYS_A3_OFF[r0], r5 /* r5 = a3 (5th arg), read through the resume USP */
    sub     #8, r14            /* the continuation header */
    mov.l   r0, 4[r14]         /* [T-4] = the USP the thread resumes on */
    mov.l   r5, [r14]          /* [T-8] = a3 */
    mov.l   r14, r0            /* on the calling thread's kernel block from here */
    bsr.a   _syscall_dispatch  /* (R1-R4, [SP]=a3) -> result in R1:R2 */
    mov.l   4[r0], r0          /* the resume USP, read before R0 leaves the block */
    /* rte pops PC+PSW only, so the R1:R2 pair reaches the caller untouched. */
    rte                        /* PC=userPC, PSW=userPSW -> back in arch_syscall (R1:R2=result) */

/* A thread whose ctx.kernel_sp is 0 reached the syscall path, so the transfer has nowhere to
   build. No TCB reaches it today, thread_create seating a block on every pool thread and idle
   being privileged and running arch_idle_wait alone, so this is a refusal for a future one
   rather than a store through 0.

   THE REPORTER RUNS ON THE ISP, and the switch is not tidiness. This site is
   supervisor-on-USP: R0 is the thread's own stack with KICKOS_RX_TRAP_REDZONE_SYS less the
   eight-byte head, 300 bytes, under it, and the reporter's console chain measures 596 on
   rx72m-st, so reporting on the USP overflows the thread's stack while printing.
   .Lsys_bad_usp and .Lpendsw_bad_usp need no such switch, both being inside a trap prologue
   already on the ISP. clrpsw is taken AFTER R0 is copied, so the reported USP is the
   thread's. The ISP here is the 8K boot/interrupt stack (_kernel_stack_size, rx72m.ld). */
/* PSW.I IS CLEARED BEFORE PSW.U, holding up an invariant the rest of the port rests on:
   I=1 IMPLIES U=1 EVERYWHERE IN arch/rx. Nothing sets I here, every setpsw/clrpsw touching u
   alone and arch_irq_save/restore moving IPL alone, so I becomes 1 only from an RTE popping a
   word that carries it, and every such word carries U too (PSW_THREAD_KERNEL and
   PSW_THREAD_USER in regs.h, arch_fault_redirect_to_exit, and .Lsys_generic's rewrite below).
   So no interrupt is ever accepted while R0 is the ISP, which is what lets kickos_rx_pendsw
   read a PM=0 entry as supervisor-on-USP off PM alone. .Lsys_generic fabricates the
   trampoline PSW from the thread's own and touches I neither way, so this arm arrives at I=1
   where the fastpath arm arrives at I=0: without this clrpsw the bank switch below is the one
   window in the port that breaks the invariant. kpanic_enter raises IPL one call later, which
   is one call too late. Noreturn, so nothing restores it. */
.Lsvc_nokstack:
    mov.l   r0, r1             /* the USP the thread would have resumed on */
    clrpsw  i
    clrpsw  u                  /* R0 = ISP */
    bsr.a   _kickos_rx_no_kernel_stack

/* arch_syscall(nr, a0, a1, a2, a3): R1=nr R2=a0 R3=a1 R4=a2, [R0+4]=a3. INT     */
/* preserves all GPRs, so no marshaling is needed. arch_syscall64 is the SAME    */
/* trap: R1:R2 is the RX ABI's 8-byte return pair.                               */
    .global _arch_syscall
    .global _arch_syscall64
_arch_syscall:
_arch_syscall64:
    int     #1                 /* SYSCALL_VECTOR (regs.h) */
    rts                        /* return to caller, R1:R2 = result */

#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* int32_t arch_syscall_reg(uint32_t* io): R1 = io. INT preserves every GPR, so  */
/* the payload rides R4-R8 straight into the trap and the trap's own frame build */
/* is what stores it. R6-R8 are callee-saved and are banked here. RTE lands after */
/* the INT whether the trap took the fastpath or ran the generic dispatch.        */
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
    /* R2-R6 carry the reply on the taken path and the untouched request on the declined one.
       The stub reads them only when the result is not KOS_CALL_REG_FALLBACK, so the
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
