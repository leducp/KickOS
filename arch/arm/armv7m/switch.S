/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv7-M context switch and syscall trap assembly: PendSV_Handler, SVC_Handler with
 * svc_trampoline, arch_start and arch_syscall, and the IPC fastpath seam. Dispatch runs in
 * PRIVILEGED THREAD mode, so a blocking syscall is an ordinary synchronous PendSV switch
 * resumed inline (arch.h arch_syscall contract). WHICH STACK it runs on is
 * KICKOS_KERNEL_STACKS, and armv7m_trap_stack.h says which chips take which.
 *
 * Saved-frame layout (must match arch_context_init in arch_armv7m.cc), low->high address on
 * the thread's PSP:
 *     [r4 r5 r6 r7 r8 r9 r10 r11]              <- PendSV-saved (ctx.sp points here)
 *     [r0 r1 r2 r3 r12 LR PC xPSR]             <- hardware exception frame
 * EXC_RETURN is ctx.exc_return and never a stack word. On an FPU part a thread with live FP
 * context also carries {s16-s31} between the callee block and the hardware frame (saved
 * before {r4-r11}, so at higher addresses) and {s0-s15,FPSCR} inside the hardware frame; both
 * are handled conditionally on EXC_RETURN bit4.
 */

#include <kickos/arch/armv7m_trap_stack.h>
/* KICKOS_KERNEL_STACK_SIZE, which PendSV_Handler compares a PSP against. */
#include <kickos/board_config.h>

    .syntax unified
    .thumb

    .equ F_CTX_STACK_LO, KICKOS_ARMV7M_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_ARMV7M_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_ARMV7M_CTX_OFF_KERNEL_SP
    .equ F_CTX_EXC_RETURN, KICKOS_ARMV7M_CTX_OFF_EXC_RETURN
    .equ F_CTX_TRACE_TID, KICKOS_ARMV7M_CTX_OFF_TRACE_TID
#if KICKOS_ARMV7M_SPLIM
    .equ F_CTX_SPLIM, KICKOS_ARMV7M_CTX_OFF_SPLIM
#endif

/* The deepest a PSP may sit inside a kernel block and still be accepted by PendSV's block leg,
   as a distance DOWN from the block's top because that is what the compare there holds. SIZE,
   less the lowest word (the overflow canary in kernel/thread/thread.cc, which no frame may
   consume), less NEST_SVCK: the room the block's own sizing reserves BELOW a dispatch for
   every frame an exception can pile on it, the PendSV save under test among them.
   AS TIGHT AS THE SIZING AND NO TIGHTER, which is what makes it safe to enforce: the deepest
   legitimate PSP is KERNEL_DEPTH_SVCK below the top, and this bound less that depth is
   (SIZE - 4) - NEED_SVCK, the block's own slack. So check_trap_redzone.sh passing is exactly
   the statement that this leg admits every legal depth. Same derivation as rxv3's
   BLOCK_USP_MAX_DROP. */
    .equ BLOCK_PSP_MAX_DROP, \
        KICKOS_KERNEL_STACK_SIZE - KICKOS_ARMV7M_TRAP_NEST_SVCK - 4

/* The frame a switch-in returns through, in a stack a task-mate can write: a stacked PC with
   bit 0 set is UNPREDICTABLE on exception return, so it is cleared in handler mode first.
   In: \f = the frame; clobbers \t. */
    .macro SCRUB_PC f, t
    ldr     \t, [\f, #24]
    bic     \t, \t, #1
    str     \t, [\f, #24]
    .endm

/* The upper leg: the whole entry frame below stack_hi. SCRUB_PC writes its PC word in handler
   mode at the switch-in, and an FP frame's {s0-s15, FPSCR} words were only RESERVED by the
   entry, filled later by the lazy save the first FP instruction here or the switcher's own
   vstmdbeq fires. Falls through when bounded. In: r0 = the PSP, r2 = the ctx*, lr =
   EXC_RETURN; clobbers r12. PSP is word-aligned, so below hi - 28 is at or below hi - 32. */
    .macro FRAME_UNDER_HI bad
    ldr     r12, [r2, #F_CTX_STACK_HI]
    sub     r12, r12, #28
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    subeq   r12, r12, #(KICKOS_ARMV7M_TRAP_ENTRY_FRAME - 32)
#endif
    cmp     r0, r12
    bhs     \bad                    /* a PSP above the stack would push into whatever
                                       granted region carried the hardware frame */
    .endm

/* PSP bounds guard, used by both software pushes in this file.
   In:  \need = the room this site's class needs below the PSP, before the FP term;
        r0 = the live PSP, r2 = the running thread's arch_context*, lr = EXC_RETURN.
   Clobbers r1 and r12, both already in the hardware frame and so free here.
   Falls through only when \need bytes, plus {s16-s31} when the FP frame is live, fit
   between stack_lo and the PSP.

   \need is the class figure armv7m_trap_stack.h derives. The two sites pass DIFFERENT ones
   because the syscall return comes back to the SVC site's PSP still privileged.

   \bad is the refusal target, and the two sites pass DIFFERENT ones because only PendSV
   spills at entry and that spill has to come back off before the shared tail runs.

   \fp charges the {s16-s31} block AT RUN TIME on an FP-live entry, and only PendSV may take
   it: its push IS that block, while the SVC figure is a structural constant that already
   dominates BOTH entry postures, so charging the term there charges it twice and refuses a
   legal floor-sized FP-active thread. */
    .macro PSP_GUARD need, fp, bad
    movw    r1, #\need
#if defined(__ARM_FP)
    .if \fp
    tst     lr, #0x10               /* bit4 == 0 => {s16-s31} goes below the PSP too */
    it      eq
    addeq   r1, r1, #KICKOS_ARMV7M_TRAP_FRAME_FP
    .endif
#endif
    ldr     r12, [r2, #F_CTX_STACK_LO]
    cmp     r0, r12
    blo     \bad
    sub     r12, r0, r12            /* the room REMAINING below the PSP: a bound written
                                       as psp - need could wrap under stack_lo instead */
    cmp     r12, r1
    blo     \bad
    FRAME_UNDER_HI \bad
    .endm

    .text

/* PendSV_Handler runs at the lowest priority, so a switch requested from an ISR happens on
   the way back to thread level. */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    ldr     r3, =g_arch_current
    ldr     r2, [r3]                /* outgoing ctx* (0 => first start) */
#if KICKOS_TELEMETRY
    /* Spill the OUTGOING ctx* before the swap clobbers r2. The telemetry tail emits from
       this value, never from a re-read of g_arch_next, which a SysTick preempting PendSV
       can rewrite. r3 keeps MSP 8-byte aligned across the later bl. */
    push    {r2, r3}
#endif
    cbz     r2, 1f                  /* nothing to save on the first entry */
    mrs     r0, psp
#if KICKOS_KERNEL_STACKS
    /* THE PSP IS EITHER STACK, and which one is not a thing to refuse. A thread preempted
       mid-dispatch is running on its own kernel block (svc_trampoline), so its PSP is inside
       [kernel_sp - KICKOS_KERNEL_STACK_SIZE, kernel_sp) and the user-stack bounds below would
       refuse every blocking syscall and kill the thread. The block leg is narrowest on
       purpose: THIS thread's kernel_sp and no other, never the whole array. An unseated block
       reads 0, so the first compare fails and the user-stack legs decide, which is what idle
       takes.

       THE ROOM IS PART OF THE RANGE. A bare range test admits a PSP at the block's base, and
       the save below then writes THROUGH THE CANARY and into the neighbouring slot's
       privileged continuation. "Every byte below a kernel PSP is privileged code's" is only
       true where the thread cannot aim its own PSP at the block, which is a board property and
       not an arch one: with enforcement the hardware refuses the entry stacking first, and on
       an armv7m board that carves blocks and does NOT enforce (the flat presets) nothing does.
       The bound stops BLOCK_PSP_MAX_DROP short of the base. */
    ldr     r1, [r2, #F_CTX_KERNEL_SP]
    subs    r1, r1, r0              /* bytes from the PSP up to the block's top */
    bls     2f                      /* at or above the top, so not on the block */
    movw    r12, #BLOCK_PSP_MAX_DROP
    cmp     r1, r12
    bls     3f                      /* on this thread's own block, a whole nest still below.
                                       Accepts the EQUAL case: the deepest legal dispatch sits
                                       exactly at the bound wherever the block has no slack. */
2:
#endif
#if KICKOS_ARMV7M_SPLIM
    /* Every PSP an entry stacked from sits at or above PSPLIM, SPLIM_ROOM over the base, and
       every stack crossing moves the limit with SP under PRIMASK: only the upper bound is left
       to software. */
    FRAME_UNDER_HI .Lpsp_wild_pendsv_hi
#else
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_PENDSV, 1, .Lpsp_wild_pendsv
#endif
#if KICKOS_KERNEL_STACKS
    /* OFF ITS OWN KERNEL BLOCK A CONTEXT IS SAVED AT ITS RESTING PRIVILEGE, so no privileged
       frame rests in memory a task-mate writes. The one privileged window on a user stack, the
       syscall return between its stack move and its privilege drop, runs correctly demoted. */
    ldr     r1, [r2, #8]            /* ctx.resting_npriv */
    b       5f
3:
    mrs     r1, control             /* the CURRENT nPRIV: a thread blocked mid-syscall */
    and     r1, r1, #1              /* is privileged and must resume so */
5:
#else
    mrs     r1, control             /* the CURRENT nPRIV: a thread blocked mid-syscall */
    and     r1, r1, #1              /* is privileged and must resume so */
#endif
#if defined(__ARM_FP)
    tst     lr, #0x10               /* EXC_RETURN bit4 == 0 => FP frame active */
    it      eq
    vstmdbeq r0!, {s16-s31}
#endif
    stmdb   r0!, {r4-r11}
    str     lr, [r2, #F_CTX_EXC_RETURN]
    str     r0, [r2]                /* outgoing->sp = r0 (ctx.sp is field 0) */
    str     r1, [r2, #4]
#if KICKOS_ARMV7M_SPLIM
    mrs     r0, psplim
    str     r0, [r2, #F_CTX_SPLIM]
#endif
1:
    /* g_arch_next and the MPU stash are ONE pair and PendSV must consume them as one: a device
       IRQ (above PRIO_PENDSV) preempting between the two reschedules and replaces BOTH, so
       one thread's PSP could be restored under the other's regions. Entry implies
       PRIMASK==0, so the cpsie below restores it exactly; the kernel lock's BASEPRI is
       untouched. */
    cpsid   i
    ldr     r1, =g_arch_next
    ldr     r1, [r1]                /* incoming ctx* */
    str     r1, [r3]                /* g_arch_current = incoming */
    ldr     r0, [r1]                /* incoming->sp */
    ldr     lr, [r1, #F_CTX_EXC_RETURN]
    ldmia   r0!, {r4-r11}
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif
    msr     psp, r0
    SCRUB_PC r0, r12
#if KICKOS_ARMV7M_SPLIM
    ldr     r12, [r1, #F_CTX_SPLIM]
    msr     psplim, r12
#endif
    ldr     r2, [r1, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = incoming npriv */
    msr     control, r3
    isb
#if KICKOS_TELEMETRY
    /* Emit {from,to} from the physically swapped contexts: r1 incoming, r2 the outgoing
       spilled at entry. from = 0xFFFF on the first switch, where the outgoing ctx* is 0. */
    pop     {r2, r3}                /* r2 = outgoing ctx* (r3 = alignment pad) */
    ldr     r1, [r1, #F_CTX_TRACE_TID] /* r1 = to_tid (incoming.trace_tid) */
    movw    r0, #0xFFFF             /* default from = no-thread */
    cmp     r2, #0
    beq     4f
    ldr     r0, [r2, #F_CTX_TRACE_TID] /* from_tid = outgoing.trace_tid */
4:
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps 8B alignment */
    bl      kickos_trace_switch_done /* (r0=from_tid, r1=to_tid) */
    pop     {r0, lr}                /* restore EXC_RETURN */
#endif
    /* Deferred MPU commit, after the physical swap: committing in arch_switch would run the
       OUTGOING thread under the incoming regions and fault it on its own stack. lr holds
       EXC_RETURN and survives the bl through the push; r0 pads MSP to 8 bytes. */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    bl      kickos_switch_landed
    pop     {r0, lr}
    clrex                           /* the incoming thread's cut STREX fails (DDI0403E A3.4.4) */
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */

/* A syscall with no current thread has no bounds to check, and the slow path below still
   writes through the PSP. The bounds go out as [0, 0), an unseated pair to the reporter.
   Never returns. */
.Lsvc_nothread:
    movw    r1, #KICKOS_ARMV7M_TRAP_NEED_SVC
    movs    r2, #0
    movs    r3, #0
    b       kickos_arm_bad_psp   /* (r0=PSP, r1=need, r2=0, r3=0) */

/* The PendSV entry alone, because .Lpsp_wild below is shared with the SVC site, which makes
   no such push and whose telemetry tail is not the one that would pop it. */
#if KICKOS_ARMV7M_SPLIM
.Lpsp_wild_pendsv_hi:
    movw    r1, #KICKOS_ARMV7M_TRAP_NEED_PENDSV
#endif
.Lpsp_wild_pendsv:
#if KICKOS_TELEMETRY
    pop     {r2, r3}                /* the entry spill: r2 is the same ctx* it already holds */
#endif

/* The PSP has no room for the software block. stack_hi is read into r3 FIRST because r2 is
   the ctx* both loads read. */
.Lpsp_wild:
#if defined(__ARM_FP)
    /* DISCARD THE OFFENDER'S PENDING LAZY FP SAVE, and do it BEFORE the call. Exception entry
       from a thread with FPCA live only RESERVES {s0-s15,FPSCR} inside the frame it allocated
       at the refused PSP: FPCCR.LSPACT is set, FPCAR points into that region, and the write is
       deferred to the next FP instruction ANY context executes. Left standing it fires long
       after the resume, flushing whatever is live then through a pointer THIS thread chose.
       The thread is dying, so the save is dropped rather than completed.
       AHEAD OF THE CALL because both things below it can carry FP: the reporter's C chain, and
       the vldmia in the restore. r12 and r3 are free here, r3 being reloaded on the next line.
       Correct on the terminating path too, where it keeps the panic dump from firing it. */
    ldr     r12, =0xE000EF34        /* FPCCR */
    ldr     r3, [r12]
    bic     r3, r3, #1              /* LSPACT */
    str     r3, [r12]
    dsb
    isb
#endif
    ldr     r3, [r2, #F_CTX_STACK_HI]
    ldr     r2, [r2, #F_CTX_STACK_LO]
#if !KICKOS_KERNEL_STACKS
    /* No block to rebuild the exit stub on, so the reporter cannot contain and never returns
       (arch_armv7m.cc says why). A tail branch rather than a call, so no fallthrough into the
       restore below exists to be reached. */
    b       kickos_arm_bad_psp   /* (r0=PSP, r1=need, r2=stack_lo, r3=stack_hi) */
#else
    bl      kickos_arm_bad_psp   /* (r0=PSP, r1=need, r2=stack_lo, r3=stack_hi) */
    /* Returns only when the offending thread was contained: it is slain and its next resume
       is claimed, and g_arch_next is the thread to run instead. Falls into the restore
       below. */

/* THE RESTORE HALF ABOVE CANNOT BE REUSED HERE, for two reasons that both come from the save
   half never having run. It expects r3 to still hold &g_arch_current, and the bl above
   clobbered it (AAPCS caller-saved); and its telemetry hook consumes a push the save half
   makes, which is not on the stack. So the sequence is repeated without those two, and the
   telemetry hook is deliberately absent: this is a fault path, not a switch to measure. lr
   held EXC_RETURN and the bl clobbered it, which costs nothing because the incoming context
   carries its own. */
.Lresume_contained:
    cpsid   i
    ldr     r3, =g_arch_current
    ldr     r2, [r3]                /* outgoing ctx* = the offender, read before the swap */
    ldr     r1, =g_arch_next
    ldr     r1, [r1]
    str     r1, [r3]                /* g_arch_current = incoming */
#if KICKOS_TELEMETRY
    /* A CONTAINED REFUSAL IS A PHYSICAL SWAP AND MUST SAY SO. kicktrace.py's verify() walks
       the SWITCH chain requiring each `from` to equal the previous `to`, so a swap that
       emitted nothing breaks the NEXT record rather than this one. Emitted before the
       register restore, which costs nothing: the record is the two contexts and not the
       register file. The push keeps MSP 8-byte aligned and carries r1 across the call; lr is
       reloaded from the incoming context below, so the bl may clobber it. */
    push    {r1, r2}
    ldr     r0, [r2, #F_CTX_TRACE_TID] /* from_tid = the offender's */
    ldr     r1, [r1, #F_CTX_TRACE_TID] /* to_tid */
    bl      kickos_trace_switch_done
    pop     {r1, r2}
#endif
    ldr     r0, [r1]                /* incoming->sp */
    ldr     lr, [r1, #F_CTX_EXC_RETURN]
    ldmia   r0!, {r4-r11}
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif
    msr     psp, r0
    SCRUB_PC r0, r12
#if KICKOS_ARMV7M_SPLIM
    ldr     r12, [r1, #F_CTX_SPLIM]
    msr     psplim, r12
#endif
    ldr     r2, [r1, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1
    msr     control, r3
    isb
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    bl      kickos_switch_landed
    pop     {r0, lr}
    clrex
    cpsie   i
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */
#endif

/* ------------------------------------------------------------------------- */
/* SVC_Handler redirects the exception return into svc_trampoline, which runs */
/* in PRIVILEGED THREAD mode and dispatches from there, so a blocking switch  */
/* is a real synchronous one (arch.h arch_syscall contract). The hardware     */
/* frame holds the args (r0=nr r1=a0 r2=a1 r3=a2 r12=a3) and, in LR, the      */
/* return address of arch_syscall's caller.                                   */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp                 /* r0 -> user hardware frame */
    /* Guards BOTH exits from here, at the SVC figure, which is the larger of the two: the
       fastpath below pushes the callee block through this PSP and returns in handler mode,
       while the slow path's return comes back to this PSP PRIVILEGED for a few instructions,
       where a preemption stacks below it. The guard sits above the `cmp #56` that picks the
       arm, so it charges whichever spends more. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]
    /* Inverted so the far branch is a plain b: cbz reaches only +126 bytes and the refusal
       is out of line. */
    cbnz    r2, 1f
    b       .Lsvc_nothread          /* no thread running, and it does not return */
1:
#if KICKOS_ARMV7M_SPLIM
    /* Every entry stacks at or above PSPLIM, whose room covers the fastpath push and the
       return window, so only the upper bound is left to software, as at PendSV. */
    movw    r1, #KICKOS_ARMV7M_TRAP_NEED_SVC
    FRAME_UNDER_HI .Lpsp_wild
#else
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_SVC, 0, .Lpsp_wild
#endif
    /* THE SEAM. KOS_SYS_CALL_REG completes here, ahead of the exception return that lands
       in svc_trampoline for the generic dispatch. The leaf decides before it mutates, so a
       null return means nothing was committed and the fall-through carries the whole call. */
    ldr     r1, [r0]                /* the stacked r0 is the syscall number */
    cmp     r1, #56                 /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     .Lsvc_slow
    /* Two refusals the leaf cannot see, both about the trap rather than the IPC. A mask
       already raised would have to be reinstated on a thread that carries none, and a pended
       PendSV means kernel().current has already moved off the thread whose registers are live
       here, with an uncommitted region stash behind it. SVCall (0xE0) tail-chains ahead of
       PendSV (0xF0), so entry in that state is reachable. */
    mrs     r1, primask
    mrs     r2, basepri
    orrs    r1, r1, r2
    bne     .Lsvc_slow
    ldr     r1, =0xE000ED04         /* SCB->ICSR */
    ldr     r1, [r1]
    tst     r1, #0x10000000         /* PENDSVSET */
    bne     .Lsvc_slow
    /* BASEPRI at the kernel-lock level, not PRIMASK: arch_irq_save takes its nested fast
       path at exactly this value, so the IrqLocks inside the leaf neither raise nor DROP the
       mask. A PRIMASK-only bracket would have the first of them restore BASEPRI to 0 and
       unmask the rest of the leaf. */
    mov     r1, #0x20               /* PRIO_LOCK_BASEPRI (regs.h) */
    msr     basepri, r1
    dsb
    isb
    /* Save the callee block exactly as PendSV does. That block IS the leaf's argument array,
       because stmdb seats r4 lowest: args[0] = r4 (the result slot the resuming switch
       writes), args[1] = r5 (endpoint cap), args[2] = r6 (packed lengths), args[3..7] =
       r7-r11 (the payload). PSP is not moved, so a refusal below leaves the words as dead
       scratch under the thread's own stack pointer. */
#if defined(__ARM_FP)
    tst     lr, #0x10               /* EXC_RETURN bit4 == 0 => FP frame active */
    it      eq
    vstmdbeq r0!, {s16-s31}
#endif
    stmdb   r0!, {r4-r11}
    push    {r0, lr}                /* the frame base and EXC_RETURN, across the bl */
    bl      kickos_ipc_fastpath
    pop     {r1, lr}
    cmp     r0, #0
    beq     .Lsvc_fp_refused
    /* Taken. r0 = the incoming thread's arch_context*, r1 = the outgoing frame base. The
       stacked PC already points past the SVC, so the caller's resume is a plain exception
       return and the switch that resumes it seats the result (sched.cc switch_book). The
       physical swap and the region commit are ONE pair: an interrupt between them would
       resume a thread under another thread's regions. PRIMASK was 0 at entry, so the cpsie
       below restores it. */
    cpsid   i
    ldr     r2, =g_arch_current
    ldr     r3, [r2]                /* outgoing ctx* (the caller) */
    str     r1, [r3]                /* outgoing->sp = this frame */
    str     lr, [r3, #F_CTX_EXC_RETURN]
    ldr     r1, [r3, #8]            /* resting: only the caller's own code reaches this path */
    str     r1, [r3, #4]
#if KICKOS_ARMV7M_SPLIM
    mrs     r1, psplim
    str     r1, [r3, #F_CTX_SPLIM]
#endif
    str     r0, [r2]                /* g_arch_current = incoming */
    ldr     r1, [r0]                /* incoming->sp */
#if KICKOS_TELEMETRY
    push    {r3, r12}               /* outgoing ctx*, across the CONTROL write below */
#endif
    ldr     lr, [r0, #F_CTX_EXC_RETURN]
    ldmia   r1!, {r4-r11}
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    vldmiaeq r1!, {s16-s31}
#endif
    msr     psp, r1
    SCRUB_PC r1, r2
#if KICKOS_ARMV7M_SPLIM
    ldr     r2, [r0, #F_CTX_SPLIM]
    msr     psplim, r2
#endif
    ldr     r2, [r0, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1
    msr     control, r3
    isb
#if KICKOS_TELEMETRY
    /* SWITCH completion hook: a physically swapped pair that emitted nothing would break
       the decoder's from == previous.to continuity check. The outgoing ctx* is always live
       here, so from_tid is read unconditionally. */
    pop     {r2, r12}               /* r2 = outgoing ctx* */
    ldr     r1, [r0, #F_CTX_TRACE_TID] /* to_tid (incoming.trace_tid) */
    ldr     r0, [r2, #F_CTX_TRACE_TID] /* from_tid */
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps MSP 8-byte aligned */
    bl      kickos_trace_switch_done
    pop     {r0, lr}
#endif
    /* Deferred MPU commit for the incoming thread: the physical swap is done and the mask
       is still up. */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    bl      kickos_switch_landed
    pop     {r0, lr}
    clrex
    cpsie   i                       /* the pair is consumed */
    mov     r1, #0
    msr     basepri, r1             /* never resume a thread under the kernel lock */
    bx      lr
.Lsvc_fp_refused:
    mov     r1, #0
    msr     basepri, r1
    mrs     r0, psp                 /* PSP never moved; the redirect below wants it back */
.Lsvc_slow:
#if KICKOS_KERNEL_STACKS
/* THE TRANSFER, built here in handler mode: the continuation header at the top T of the
   caller's kernel block, and below it the frame the exception return pops into
   svc_trampoline. No word that steers this privileged return is read back from the user
   frame, which a task-mate can rewrite: only the arguments and the caller-return are copied.
   In: r0 = the guarded user frame, lr = its EXC_RETURN. The ctx* is reloaded here, never
   taken from a register: the refusals above branch in with r2 spent.

   THE CONTINUATION HEADER, read back by the trampoline after the call:
       [T-16]  a3, the AAPCS fifth argument, which syscall_dispatch reads at [sp]
       [T-12]  the caller-return this exits to
       [T-8]   the PSP the thread resumes on
       [T-4]   the word that keeps sp 8-byte aligned for the call; with a stack limit, the
               caller's PSPLIM, put back on the way out
   The resume PSP lives HERE and not in a register or a second context field: a blocking
   dispatch has PendSV freeze its continuation BELOW this header, so the header survives the
   block for free, and ctx.sp names whichever stack the thread was preempted on.

   MASKED TO THE RETURN, and the trampoline unmasks. An FP-live caller's frame is extended, and
   its {s0-s15, FPSCR} words are popped from the built frame unless FPCCR.LSPACT is still set
   at the return, so the copy below must see the LSPACT the return will; PRIMASK was 0 at
   entry, an SVC being untakeable under it. */
    cpsid   i
    ldr     r3, =g_arch_current
    ldr     r3, [r3]
    ldr     r3, [r3, #F_CTX_KERNEL_SP]
    cmp     r3, #0
    beq.w   .Lsvc_nokstack          /* no block to build on, and it does not return */
    add     r1, r0, #32             /* the PSP the thread resumes on, past the popped frame */
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    addeq   r1, r1, #72
#endif
    ldr     r12, [r0, #28]
    tst     r12, #0x200             /* and the realign word the entry spent */
    it      ne
    addne   r1, r1, #4
    str     r1, [r3, #-8]
    ldr     r12, [r0, #20]
    str     r12, [r3, #-12]
    ldr     r12, [r0, #16]
    str     r12, [r3, #-16]
#if KICKOS_ARMV7M_SPLIM
    mrs     r12, psplim
    str     r12, [r3, #-4]
    movw    r12, #(KICKOS_KERNEL_STACK_SIZE - KICKOS_ARMV7M_SPLIM_ROOM)
    sub     r12, r3, r12
    msr     psplim, r12             /* before the PSP, which lands above it */
#endif
    sub     r3, r3, #48             /* the header and a basic frame */
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    subeq   r3, r3, #72             /* the same frame shape the entry stacked */
#endif
    ldrd    r1, r12, [r0]           /* r0, r1 */
    strd    r1, r12, [r3]
    ldrd    r1, r12, [r0, #8]       /* r2, r3 */
    strd    r1, r12, [r3, #8]
    ldrd    r1, r12, [r0, #16]      /* r12, lr */
    strd    r1, r12, [r3, #16]
    ldr     r1, =svc_trampoline
    bic     r1, r1, #1
    mov     r12, #0x01000000        /* T, and no IT, ICI, IPSR or realign bit */
#if defined(__ARM_FP) && defined(__ARM_FEATURE_CMSE)
    /* RETPSR.SFPA, which a return to Secure state puts back in CONTROL_S.SFPA: left clear
       under an FP frame, the caller's next FP instruction reloads FPSCR from FPDSCR. */
    and     r2, lr, #0x50
    cmp     r2, #0x40               /* EXC_RETURN.S set, FType clear */
    it      eq
    orreq   r12, r12, #0x00100000
#endif
    strd    r1, r12, [r3, #24]
#if defined(__ARM_FP)
    tst     lr, #0x10
    bne     2f
    ldr     r12, =0xE000EF34        /* FPCCR */
    ldr     r12, [r12]
    tst     r12, #1                 /* LSPACT */
    bne     2f
    add     r0, r0, #32
    add     r1, r3, #32
    movs    r2, #17                 /* {s0-s15}, FPSCR */
1:
    ldr     r12, [r0], #4
    str     r12, [r1], #4
    subs    r2, r2, #1
    bne     1b
2:
#endif
    msr     psp, r3
    mrs     r1, control
    bic     r1, r1, #1              /* CONTROL.nPRIV = 0 -> trampoline privileged */
    msr     control, r1
    isb
    bx      lr                      /* -> svc_trampoline on the block, r0-r3 the arguments */

/* A thread whose ctx.kernel_sp is 0 reached the syscall path, so the transfer has nowhere to
   build; refusing here replaces a store through 0xFFFFFFF0. In handler mode, so the reporter
   runs on the MSP. r0 is the PSP the thread would have resumed on. Noreturn, so no cpsie pairs
   with the mask above. */
.Lsvc_nokstack:
    add     r0, r0, #32
    b       kickos_arm_no_kernel_stack
#else
    ldr     r1, =svc_trampoline
    bic     r1, r1, #1              /* stacked PC must be halfword-aligned (bit0=0;
                                       the T-bit comes from the stacked xPSR) */
    str     r1, [r0, #24]           /* frame.PC := svc_trampoline (offset 6*4) */
    /* T and the realign bit only: IT/ICI left as the caller set them would skip the
       trampoline's first instructions. */
    ldr     r1, [r0, #28]
    and     r1, r1, #0x200
    orr     r1, r1, #0x01000000
    str     r1, [r0, #28]
#endif
    mrs     r1, control
    bic     r1, r1, #1              /* CONTROL.nPRIV = 0 -> trampoline privileged */
    msr     control, r1
    isb
    bx      lr

    .thumb_func
    .global svc_trampoline
svc_trampoline:
#if KICKOS_KERNEL_STACKS
/* Entered privileged in THREAD mode on the caller's kernel block, sp at the continuation
   header, masked by the slow path. The drop to the caller's resting privilege comes AFTER sp is
   back on the thread's own stack: the header is kernel .bss and an unprivileged read of it
   faults. */
    cpsie   i
#if defined(KICKOS_SPLIM_PLANT_KBLOCK)
    bl      kickos_splim_plant      /* test-only: overflow this kernel block, privileged */
#endif
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* the caller-return */
    ldr     r2, [sp, #8]            /* the resume PSP, read before sp leaves this block */
#if KICKOS_ARMV7M_SPLIM
    ldr     r3, [sp, #12]           /* the caller's limit */
#endif
    /* The caller's RESTING privilege, so a privileged thread issuing a syscall keeps its
       posture, read here because nothing after the stack move may need privilege: a
       preemption there is saved demoted. g_arch_current is this thread again, PendSV having
       restored it across any block. r0:r1 carry the result pair (AAPCS long long) out. */
    ldr     r12, =g_arch_current
    ldr     r12, [r12]
    ldr     r12, [r12, #8]          /* ctx.resting_npriv */
#if KICKOS_ARMV7M_SPLIM
    cpsid   i
    msr     psplim, r3
    mov     sp, r2
    cpsie   i
#else
    mov     sp, r2
#endif
kickos_armv7m_svc_drop:
    mrs     r3, control
    bfi     r3, r12, #0, #1         /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */
#else
/* THE UNCONVERTED DESIGN, for the armv7m chips with no MPU that keep their red zone
   (armv7m_trap_stack.h names them). syscall_dispatch runs on the calling thread's own PSP,
   which the SVC site refused unless KICKOS_ARMV7M_TRAP_NEED_SVC fits below it, and a
   blocking dispatch has PendSV freeze the continuation on that same thread stack. */
    push    {r12, lr}               /* [sp]=a3 (5th arg), [sp,#4]=caller-return */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* restore caller-return */
    add     sp, sp, #8
    /* The caller's RESTING privilege, as in the transfer arm above: r2/r3 only, r0:r1
       carrying the result pair out. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]                /* current ctx* */
    ldr     r2, [r2, #8]            /* ctx.resting_npriv (offset 8) */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */
#endif

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first): enter the first thread from boot (thread/MSP) by  */
/* reusing the PendSV switch path. PendSV fires once sched::start() releases  */
/* its IrqLock (BASEPRI); boot's MSP frame is abandoned.                      */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_start
arch_start:                         /* r0=boot (ignored on MCU), r1=first */
    ldr     r2, =g_arch_current
    movs    r3, #0
    str     r3, [r2]                /* g_arch_current = 0 (first-start sentinel) */
    ldr     r2, =g_arch_next
    str     r1, [r2]                /* g_arch_next = first */
    ldr     r2, =0xE000ED04         /* SCB->ICSR */
    ldr     r3, =0x10000000         /* PENDSVSET */
    str     r3, [r2]
    dsb
    isb
    bx      lr

/* kickos_panic_stack_enter(msg, file, line, top): leave the interrupted stack for the
   reporter's own (arch.h). r0-r2 pass through untouched; r3 is the seat.
   PRIMASK before the move, or an exception taken between the two stacks on the stack this
   exists to leave; it is stronger than the BASEPRI arch_irq_save uses and this path does not
   return. `b` and not `bl`: the -fcallgraph-info walk check_trap_redzone.sh runs stops at a
   body with no call edge, which is what takes the console tail out of every red zone. Thread
   mode does not change SPSEL, so this writes whichever of MSP and PSP was live. */
#if KICKOS_KERNEL_CORES > 1
#error "one kernel over several armv7m cores has no per-core panic seat here"
#endif
    .thumb_func
    .global kickos_panic_stack_enter
kickos_panic_stack_enter:
    cpsid   i
#if KICKOS_ARMV7M_SPLIM
    /* The live stack's limit to the seat's base, before SP moves: the seat may lie above it.
       Handler mode runs on the MSP whatever CONTROL.SPSEL holds. */
    mrs     r12, ipsr
    cmp     r12, #0
    bne     1f
    mrs     r12, control
    tst     r12, #2
    beq     1f
    movw    r12, #KICKOS_PANIC_STACK_SIZE
    sub     r12, r3, r12
    msr     psplim, r12
    b       2f
1:
    movw    r12, #KICKOS_PANIC_STACK_SIZE
    sub     r12, r3, r12
    msr     msplim, r12
2:
#endif
    mov     sp, r3
    b       kickos_panic_report

    .end
