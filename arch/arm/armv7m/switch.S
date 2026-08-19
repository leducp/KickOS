/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv7-M context switch + syscall trap assembly. Three mechanisms live here:
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

    .syntax unified
    .thumb
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
       caller-saved (HW-stacked), free to clobber here. */
    ldr     r0, =0xE0001004
    ldr     r0, [r0]
    ldr     r1, =g_bench_sw_start
    str     r0, [r1]
#endif
    ldr     r3, =g_arch_current
    ldr     r2, [r3]                /* outgoing ctx* (0 => first start) */
#if KICKOS_TELEMETRY
    /* Spill the OUTGOING ctx* now, before the swap clobbers r2: the telemetry
       tail emits from this spilled value and the incoming ctx* it actually loads,
       never re-reading g_arch_next (a SysTick preempting PendSV can rewrite it).
       r3 is pushed only to keep MSP 8-byte aligned across the later bl. */
    push    {r2, r3}
#endif
    cbz     r2, 1f                  /* nothing to save on the first entry */
    mrs     r0, psp
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
       kernel lock; PendSV must consume them as one. A device IRQ (PRIO_DEVICE, above
       PRIO_PENDSV) preempting between the two reschedules and replaces BOTH, so an
       unmasked PendSV can restore one thread's PSP and then program the other thread's
       regions -> the resumed thread faults on its own stack. PRIMASK masks PendSV
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
    /* Bench: delta = now - entry stamp -> kickos_bench_switch_done. Read here,
       right after the switch body, BEFORE any telemetry emit, so the measured
       window brackets only the save/restore, not the emit. r1 (incoming ctx*) and
       the entry-spilled {r2,r3} below it are preserved across the balanced push/pop
       (telemetry reads them next). Keep MSP 8-byte aligned; save EXC_RETURN. */
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
    /* SWITCH completion hook. Emit {from,to} from the PHYSICALLY-swapped contexts:
       r1 is the incoming ctx* we loaded above; r2 (popped) is the outgoing ctx*
       we spilled at entry. trace_tid lives at offset 12. from = 0xFFFF on the
       first switch (outgoing ctx* was 0). EXC_RETURN (lr) is saved across the bl,
       MSP kept 8-byte aligned. */
    pop     {r2, r3}                /* r2 = outgoing ctx* (r3 = alignment pad) */
    ldr     r1, [r1, #12]           /* r1 = to_tid (incoming.trace_tid) */
    movw    r0, #0xFFFF             /* default from = no-thread */
    cmp     r2, #0
    beq     4f
    ldr     r0, [r2, #12]           /* from_tid = outgoing.trace_tid */
4:
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps 8B alignment */
    bl      kickos_trace_switch_done /* (r0=from_tid, r1=to_tid) */
    pop     {r0, lr}                /* restore EXC_RETURN */
#endif
    /* Deferred MPU commit: program the incoming thread's regions now that the physical
       PSP/register swap is done, NOT eagerly in switch_to (shared seam, see
       arch_arm_common.cc). Eager apply would run the OUTGOING thread under the incoming
       region set until PendSV fires -> a fault on its own stack. Callee regs (r4-r11)
       survive the call (AAPCS); r0-r3/r12 are reloaded from the HW frame on exception
       return; lr holds EXC_RETURN and must be preserved across the bl (the r0 pad keeps
       MSP 8-byte aligned). kickos_arch_mpu_commit resolves to the PMSAv7 fallback, a
       chip override (K64F SYSMPU), or an empty fallback on a no-MPU board. */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */

/* ------------------------------------------------------------------------- */
/* SVC_Handler: redirect the exception return into svc_trampoline, which runs */
/* in PRIVILEGED THREAD mode. It does NOT dispatch (that would run in handler */
/* mode, where a blocking switch could only be deferred, breaking the         */
/* synchronous-blocking contract). The hardware exception frame already holds */
/* the args (r0=nr, r1=a0, r2=a1, r3=a2, r12=a3) and, in LR, the return       */
/* address of arch_syscall's caller, so only the stacked PC is rewritten.     */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp                 /* r0 -> user hardware frame */
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    /* THE SEAM. KOS_SYS_CALL_REG completes here, before the exception return that would
       otherwise land in svc_trampoline and run the generic dispatch. The leaf decides
       before it mutates, so a null return means nothing was committed and the
       fall-through below is today's path exactly. */
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
       return and the switch that resumes it seats the result (sched.cc switch_book). */
    /* The physical swap and the region commit are ONE pair, exactly as in the PendSV
       epilogue: an interrupt between them would resume a thread under another thread's
       regions. PRIMASK was 0 at entry (refused otherwise), so the cpsie below restores it. */
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
    /* SWITCH completion hook, as in the PendSV epilogue: a physically swapped pair that
       emitted nothing would break the decoder's from == previous.to continuity check. The
       outgoing ctx* is never 0 here, so there is no first-switch case to default. */
    pop     {r2, r12}               /* r2 = outgoing ctx* */
    ldr     r1, [r0, #12]           /* to_tid (incoming.trace_tid) */
    ldr     r0, [r2, #12]           /* from_tid */
    push    {r0, lr}                /* save EXC_RETURN; r0 pad keeps MSP 8-byte aligned */
    bl      kickos_trace_switch_done
    pop     {r0, lr}
#endif
    /* Deferred MPU commit for the incoming thread, the same seam and the same reason as
       the PendSV epilogue: the physical swap is done and the mask is still up. */
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
/* inline when the thread is next scheduled. Privilege is dropped with a plain */
/* CONTROL write (no trap is needed to LOWER privilege in thread mode) before  */
/* returning to the user.                                                      */
    .thumb_func
    .global svc_trampoline
svc_trampoline:
    push    {r12, lr}               /* [sp]=a3 (5th arg), [sp,#4]=caller-return */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* restore caller-return */
    add     sp, sp, #8
    /* Restore the caller's RESTING privilege (not a hard-coded nPRIV=1): a
       privileged thread issuing a syscall must not be demoted. g_arch_current
       is this thread again here (PendSV restores it across any block).
       r2/r3 only: r0:r1 carry the result pair (AAPCS long long) all the way
       out of this trampoline, so neither may be used as scratch here. */
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
       SVC and the declined one: svc_trampoline exits to the STACKED LR, not past the SVC,
       so a wrapper cannot resume in line the way the RISC-V trampoline lets it. The taken
       path restores the hardware frame and so resumes at the SVC's own return address. */
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
