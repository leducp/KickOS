/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv7-M context switch + syscall trap assembly. Four mechanisms live here:
 *   1. PendSV_Handler: the deferred context switch (the arch_switch backend).
 *   2. SVC_Handler + svc_trampoline: the syscall trap. Dispatch runs in PRIVILEGED
 *      THREAD mode, so a blocking syscall is an ordinary synchronous PendSV switch
 *      resumed inline (arch.h arch_syscall contract).
 *   3. arch_start / arch_syscall: first-thread entry and the user-side trap.
 *   4. The IPC fastpath seam inside SVC_Handler, and arch_syscall_reg, which carries
 *      its payload in the callee block because the hardware frame is only five words.
 *
 * Saved-frame layout (must match arch_context_init in arch_armv7m.cc), low->high
 * address on the thread's PSP:
 *     [r4 r5 r6 r7 r8 r9 r10 r11 EXC_RETURN]   <- PendSV-saved (ctx.sp points here)
 *     [r0 r1 r2 r3 r12 LR PC xPSR]             <- hardware exception frame
 * On an FPU part a thread with live FP context also carries {s16-s31} between the
 * callee block and the hardware frame (saved before {r4-r11,lr}, so at higher
 * addresses) and {s0-s15,FPSCR} inside the hardware frame; both are handled
 * conditionally on EXC_RETURN bit4.
 */

#include <kickos/arch/armv7m_trap_stack.h>

    .syntax unified
    .thumb

    .equ F_CTX_STACK_LO, KICKOS_ARMV7M_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_ARMV7M_CTX_OFF_STACK_HI

/* PSP bounds guard, used by both software pushes in this file.
   In:  \need = the room this site's class needs below the PSP, before the FP term;
        r0 = the live PSP, r2 = the running thread's arch_context*, lr = EXC_RETURN.
   Clobbers r1 and r12, both already in the hardware frame and so free here.
   Falls through only when \need bytes, plus {s16-s31} when the FP frame is live, fit
   between stack_lo and the PSP.

   Exception entry stacks the hardware frame ABOVE the PSP with the PRE-EXCEPTION
   privilege, so the MPU refuses that half and a kernel-aimed PSP faults as MSTKERR
   before any handler runs. The block pushed BELOW the PSP is written in handler mode
   and is refused by nothing, so a PSP 32 bytes above a stack's base clears the
   hardware check and still writes under the stack. Its last word is the EXC_RETURN the
   resume branches through, so a neighbour owning that word and writing 0xFFFFFFF1
   resumes the victim in handler mode, privileged.

   \need covers the software push AND whatever kernel C then descends below it on this
   same stack, per class, as armv7m_trap_stack.h derives it. The two sites pass DIFFERENT
   figures because only the SVC slow path leaves handler mode and keeps running on the
   thread's PSP. */
    .macro PSP_GUARD need
    movw    r1, #\need
#if defined(__ARM_FP)
    tst     lr, #0x10               /* bit4 == 0 => {s16-s31} goes below the PSP too */
    it      eq
    addeq   r1, r1, #KICKOS_ARMV7M_TRAP_FRAME_FP
#endif
    ldr     r12, [r2, #F_CTX_STACK_LO]
    cmp     r0, r12
    blo     .Lpsp_wild
    sub     r12, r0, r12            /* the room REMAINING below the PSP: a bound written
                                       as psp - need could wrap under stack_lo instead */
    cmp     r12, r1
    blo     .Lpsp_wild
    ldr     r12, [r2, #F_CTX_STACK_HI]
    cmp     r0, r12
    bhs     .Lpsp_wild              /* a PSP above the stack would push into whatever
                                       granted region carried the hardware frame */
    .endm

    .text

/* ------------------------------------------------------------------------- */
/* PendSV_Handler: switch from g_arch_current to g_arch_next. Lowest priority */
/* so it runs after every other exception tail-chains; a switch requested from */
/* an ISR happens on the way back to thread level.                            */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
#if KICKOS_BENCH
    /* Bench: timestamp the switch entry (DWT CYCCNT @ 0xE0001004). r0/r1 are
       caller-saved and already HW-stacked, so they are free here. */
    ldr     r0, =0xE0001004
    ldr     r0, [r0]
    ldr     r1, =g_bench_sw_start
    str     r0, [r1]
#endif
    ldr     r3, =g_arch_current
    ldr     r2, [r3]                /* outgoing ctx* (0 => first start) */
#if KICKOS_TELEMETRY
    /* Spill the OUTGOING ctx* before the swap clobbers r2: the telemetry tail emits
       from this value and the incoming ctx* it loads, never from a re-read of
       g_arch_next, which a SysTick preempting PendSV can rewrite. r3 keeps MSP 8-byte
       aligned across the later bl. */
    push    {r2, r3}
#endif
    cbz     r2, 1f                  /* nothing to save on the first entry */
    mrs     r0, psp
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_PENDSV
#if defined(__ARM_FP)
    tst     lr, #0x10               /* EXC_RETURN bit4 == 0 => FP frame active */
    it      eq
    vstmdbeq r0!, {s16-s31}
#endif
    stmdb   r0!, {r4-r11, lr}       /* push callee regs + EXC_RETURN */
    str     r0, [r2]                /* outgoing->sp = r0 (ctx.sp is field 0) */
    mrs     r0, control             /* save the outgoing thread's CURRENT */
    and     r0, r0, #1              /* privilege (nPRIV): a thread blocked mid- */
    str     r0, [r2, #4]            /* syscall is privileged and must resume so */
1:
    /* g_arch_next and the deferred MPU stash are ONE pair, written together under the
       kernel lock, and PendSV must consume them as one. A device IRQ (PRIO_DEVICE, above
       PRIO_PENDSV) preempting between the two reschedules and replaces BOTH, so an
       unmasked PendSV can restore one thread's PSP and then program the other thread's
       regions, and the resumed thread faults on its own stack. PRIMASK masks PendSV
       itself, so entry here implies PRIMASK==0 and the cpsie below restores the entry
       state exactly; the kernel lock uses BASEPRI and is untouched by this. */
    cpsid   i
    ldr     r1, =g_arch_next
    ldr     r1, [r1]                /* incoming ctx* */
    str     r1, [r3]                /* g_arch_current = incoming */
    ldr     r0, [r1]                /* incoming->sp */
    ldmia   r0!, {r4-r11, lr}       /* pop callee regs + EXC_RETURN */
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
#endif
    msr     psp, r0
    ldr     r2, [r1, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = incoming npriv */
    msr     control, r3
    isb
#if KICKOS_BENCH
    /* Bench: delta = now - entry stamp -> kickos_bench_switch_done. Read right after
       the switch body and BEFORE any telemetry emit, so the measured window brackets
       the save/restore alone. The balanced push/pop preserves r1 (incoming ctx*) and
       the entry-spilled {r2,r3} beneath it, which telemetry reads next. Keeps MSP
       8-byte aligned and saves EXC_RETURN. */
    push    {r0, r1, r2, lr}
    ldr     r0, =0xE0001004
    ldr     r0, [r0]
    ldr     r1, =g_bench_sw_start
    ldr     r1, [r1]
    subs    r0, r0, r1
    bl      kickos_bench_switch_done
    pop     {r0, r1, r2, lr}
#endif
#if KICKOS_TELEMETRY
    /* SWITCH completion hook. Emit {from,to} from the PHYSICALLY-swapped contexts: r1
       is the incoming ctx* loaded above, r2 the outgoing ctx* spilled at entry.
       from = 0xFFFF on the first switch, where the outgoing ctx* is 0. EXC_RETURN (lr)
       is saved across the bl and MSP kept 8-byte aligned. */
    pop     {r2, r3}                /* r2 = outgoing ctx* (r3 = alignment pad) */
    ldr     r1, [r1, #20]           /* r1 = to_tid (incoming.trace_tid) */
    movw    r0, #0xFFFF             /* default from = no-thread */
    cmp     r2, #0
    beq     4f
    ldr     r0, [r2, #20]           /* from_tid = outgoing.trace_tid */
4:
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps 8B alignment */
    bl      kickos_trace_switch_done /* (r0=from_tid, r1=to_tid) */
    pop     {r0, lr}                /* restore EXC_RETURN */
#endif
    /* Deferred MPU commit: the incoming thread's regions are programmed here, after the
       physical PSP/register swap (shared seam, arch_arm_common.cc). Applying them in
       switch_to instead would run the OUTGOING thread under the incoming region set
       until PendSV fires, faulting it on its own stack. Callee regs (r4-r11) survive
       the call (AAPCS), r0-r3/r12 are reloaded from the HW frame on exception return,
       and lr holds EXC_RETURN and must be preserved across the bl (the r0 pad keeps MSP
       8-byte aligned). */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */

/* The PSP has no room for the software block. r3 is loaded FIRST because r2 is the ctx*
   both loads read. Never returns. */
.Lpsp_wild:
    ldr     r3, [r2, #F_CTX_STACK_HI]
    ldr     r2, [r2, #F_CTX_STACK_LO]
    b       kickos_armv7m_bad_psp   /* (r0=PSP, r1=need, r2=stack_lo, r3=stack_hi) */

/* ------------------------------------------------------------------------- */
/* SVC_Handler: redirect the exception return into svc_trampoline, which runs */
/* in PRIVILEGED THREAD mode and dispatches from there, so a blocking switch  */
/* is a real synchronous one (arch.h arch_syscall contract). The hardware      */
/* exception frame already holds the args (r0=nr, r1=a0, r2=a1, r3=a2, r12=a3) */
/* and, in LR, the return address of arch_syscall's caller, so only the        */
/* stacked PC is rewritten.                                                   */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp                 /* r0 -> user hardware frame */
    /* Guards BOTH exits from here, at the SVC figure, which is the larger of the two:
       the fastpath below pushes the callee block through this PSP and returns in handler
       mode, while the slow path exception-returns into svc_trampoline, which runs
       PRIVILEGED IN THREAD MODE on this same PSP and calls syscall_dispatch below it.
       The guard sits above the `cmp #56` that picks the arm, so it charges whichever
       descends further. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]
    cbz     r2, .Lsvc_nothread     /* no thread running: nothing to bound against */
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_SVC
.Lsvc_nothread:
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    /* THE SEAM. KOS_SYS_CALL_REG completes here, ahead of the exception return that
       lands in svc_trampoline for the generic dispatch. The leaf decides before it
       mutates, so a null return means nothing was committed and the fall-through below
       carries the whole call. */
    ldr     r1, [r0]                /* the stacked r0 is the syscall number */
    cmp     r1, #56                 /* KOS_SYS_CALL_REG (sys/abi.h) */
    bne     .Lsvc_slow
    /* Two refusals the leaf cannot see, both about the trap rather than the IPC.
       A mask already raised would have to be reinstated on a thread that carries none,
       and a pended PendSV means kernel().current has already moved off the thread whose
       registers are live here, with an uncommitted region stash behind it. SVCall (0xE0)
       tail-chains ahead of PendSV (0xF0), so entry in that state is reachable. */
    mrs     r1, primask
    mrs     r2, basepri
    orrs    r1, r1, r2
    bne     .Lsvc_slow
    ldr     r1, =0xE000ED04         /* SCB->ICSR */
    ldr     r1, [r1]
    tst     r1, #0x10000000         /* PENDSVSET */
    bne     .Lsvc_slow
    /* BASEPRI at the kernel-lock level, not PRIMASK: arch_irq_save takes its nested fast
       path at exactly this value, so the IrqLocks inside the leaf neither raise nor DROP
       the mask. A PRIMASK-only bracket would have the first of them restore BASEPRI to 0
       and unmask the rest of the leaf. */
    mov     r1, #0x20               /* PRIO_LOCK_BASEPRI (regs.h) */
    msr     basepri, r1
    dsb
    isb
    /* Save the callee block exactly as PendSV does. That block IS the leaf's argument
       array, because stmdb seats r4 lowest: args[0] = r4 (the result slot the resuming
       switch writes), args[1] = r5 (endpoint cap), args[2] = r6 (packed lengths),
       args[3..7] = r7-r11 (the payload). PSP is not moved, so a refusal below leaves the
       words as dead scratch under the thread's own stack pointer. */
#if defined(__ARM_FP)
    tst     lr, #0x10               /* EXC_RETURN bit4 == 0 => FP frame active */
    it      eq
    vstmdbeq r0!, {s16-s31}
#endif
    stmdb   r0!, {r4-r11, lr}
    push    {r0, lr}                /* the frame base and EXC_RETURN, across the bl */
    bl      kickos_ipc_fastpath
    pop     {r1, lr}
    cmp     r0, #0
    beq     .Lsvc_fp_refused
    /* Taken. r0 = the incoming thread's arch_context*, r1 = the outgoing frame base. The
       stacked PC already points past the SVC, so the caller's resume is a plain exception
       return and the switch that resumes it seats the result (sched.cc switch_book).
       The physical swap and the region commit are ONE pair: an interrupt between them
       would resume a thread under another thread's regions. PRIMASK was 0 at entry, so
       the cpsie below restores it. */
    cpsid   i
    ldr     r2, =g_arch_current
    ldr     r3, [r2]                /* outgoing ctx* (the caller) */
    str     r1, [r3]                /* outgoing->sp = this frame */
    mrs     r1, control
    and     r1, r1, #1              /* the caller's own nPRIV: this path never raised it */
    str     r1, [r3, #4]
    str     r0, [r2]                /* g_arch_current = incoming */
    ldr     r1, [r0]                /* incoming->sp */
#if KICKOS_TELEMETRY
    push    {r3, r12}               /* outgoing ctx*, across the CONTROL write below */
#endif
    ldmia   r1!, {r4-r11, lr}
#if defined(__ARM_FP)
    tst     lr, #0x10
    it      eq
    vldmiaeq r1!, {s16-s31}
#endif
    msr     psp, r1
    ldr     r2, [r0, #4]            /* incoming->npriv */
    mrs     r3, control
    bfi     r3, r2, #0, #1
    msr     control, r3
    isb
#if KICKOS_TELEMETRY
    /* SWITCH completion hook: a physically swapped pair that emitted nothing would break
       the decoder's from == previous.to continuity check. The outgoing ctx* is always
       live here, so from_tid is read unconditionally. */
    pop     {r2, r12}               /* r2 = outgoing ctx* */
    ldr     r1, [r0, #20]           /* to_tid (incoming.trace_tid) */
    ldr     r0, [r2, #20]           /* from_tid */
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps MSP 8-byte aligned */
    bl      kickos_trace_switch_done
    pop     {r0, lr}
#endif
    /* Deferred MPU commit for the incoming thread: the physical swap is done and the
       mask is still up. */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* the pair is consumed */
    mov     r1, #0
    msr     basepri, r1             /* never resume a thread under the kernel lock */
    bx      lr
.Lsvc_fp_refused:
    mov     r1, #0
    msr     basepri, r1
    mrs     r0, psp                 /* PSP never moved; the redirect below wants it back */
.Lsvc_slow:
#endif
    ldr     r1, =svc_trampoline
    bic     r1, r1, #1              /* stacked PC must be halfword-aligned (bit0=0;
                                       the T-bit comes from the stacked xPSR) */
    str     r1, [r0, #24]           /* frame.PC := svc_trampoline (offset 6*4) */
    mrs     r1, control
    bic     r1, r1, #1              /* CONTROL.nPRIV = 0 -> trampoline privileged */
    msr     control, r1
    isb
    bx      lr                      /* exception-return into svc_trampoline:
                                       r0=nr r1=a0 r2=a1 r3=a2 r12=a3
                                       LR=arch_syscall caller-return */

/* Runs in privileged thread mode on the calling thread's own PSP stack. A    */
/* blocking syscall_dispatch here blocks by pending PendSV (arch_switch); the  */
/* switch is a real exception preempting this thread-mode code, and PendSV     */
/* freezes the mid-dispatch continuation on THIS thread's stack, resuming it   */
/* inline when the thread is next scheduled. Thread mode can LOWER privilege   */
/* with a plain CONTROL write, so the drop before returning to the user is     */
/* that write.                                                                 */
    .thumb_func
    .global svc_trampoline
svc_trampoline:
    push    {r12, lr}               /* [sp]=a3 (5th arg), [sp,#4]=caller-return */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* restore caller-return */
    add     sp, sp, #8
    /* Restore the caller's RESTING privilege, so a privileged thread issuing a syscall
       keeps its posture. g_arch_current is this thread again here, PendSV having
       restored it across any block. r2/r3 only: r0:r1 carry the result pair (AAPCS
       long long) all the way out of this trampoline. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]                /* current ctx* */
    ldr     r2, [r2, #8]            /* ctx.resting_npriv (offset 8) */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first): enter the first thread from boot (thread/MSP).    */
/* Reuses the PendSV switch path: mark "no outgoing to save" and pend PendSV.  */
/* PendSV fires once sched::start() releases its IrqLock (BASEPRI), loading    */
/* `first`; boot's MSP frame is abandoned (the system never returns to boot).  */
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

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3): userspace trap (runs unprivileged).      */
/* AAPCS: r0=nr r1=a0 r2=a1 r3=a2, [sp]=a3. Move a3 into r12 so the exception  */
/* frame carries it; the result is delivered in r0:r1 by svc_trampoline, which */
/* returns straight to this function's caller (the trailing bx is a fallback). */
/* arch_syscall64 is the SAME trap: r0:r1 is the AAPCS long long pair.        */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_syscall64
arch_syscall64:
    .thumb_func
    .global arch_syscall
arch_syscall:
    ldr     r12, [sp]               /* a3 (5th arg) -> stacked r12 */
    svc     #0
    bx      lr

#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* ------------------------------------------------------------------------- */
/* int32_t arch_syscall_reg(uint32_t* io): the register-carrying IPC trap.    */
/* The hardware frame holds only five words, so the payload rides the CALLEE  */
/* block instead: io[1..7] load r5-r11, which SVC_Handler pushes contiguously */
/* below the frame for the kernel leaf to read and write. r0 keeps the number */
/* so a refusal reaching syscall_dispatch still answers on it. On return      */
/* io[1..5] take back r5-r9 (the reply) and r4 is the result.                 */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_syscall_reg
arch_syscall_reg:
    push    {r3-r11, lr}            /* the r3 slot holds `io` across the trap */
    str     r0, [sp]
    adds    r1, r0, #4
    ldr     r0, [r0]                /* nr */
    ldm     r1, {r5-r11}
    /* The two outcomes land at two different addresses, and nothing may run between the
       SVC and the declined one: svc_trampoline exits to the STACKED LR, so the declined
       arm resumes at whatever LR holds here. The taken path restores the hardware frame
       and resumes at the SVC's own return address. */
    adr     r1, .Lreg_declined
    orr     r1, r1, #1              /* the trampoline exits with bx: Thumb bit required */
    mov     lr, r1
    svc     #0
    ldr     r3, [sp]                /* taken: r4 = result, r5-r9 = the reply */
    adds    r2, r3, #4
    stmia   r2, {r5-r9}
    mov     r0, r4
.Lreg_declined:                     /* r0 = the generic dispatch's KOS_CALL_REG_FALLBACK */
    pop     {r3-r11, pc}
#endif

    .end
