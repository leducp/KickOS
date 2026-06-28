/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv7-M context switch + syscall trap assembly. Three mechanisms live here:
 *   1. PendSV_Handler  -- the deferred context switch (the arch_switch backend).
 *   2. SVC_Handler + svc_trampoline -- the syscall trap. THE M1 spike: dispatch
 *      runs in PRIVILEGED THREAD mode, so a blocking syscall is an ordinary
 *      synchronous PendSV switch resumed inline (arch.h arch_syscall contract).
 *   3. arch_start / arch_syscall -- first-thread entry and the user-side trap.
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
/* PendSV_Handler -- switch from g_arch_current to g_arch_next.               */
/* Lowest priority: it runs after every other exception tail-chains, so a     */
/* switch requested from an ISR happens on the way back to thread level.      */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    ldr     r3, =g_arch_current
    ldr     r2, [r3]                /* outgoing ctx* (0 => first start) */
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
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */

/* ------------------------------------------------------------------------- */
/* SVC_Handler -- redirect the exception return into svc_trampoline running   */
/* in PRIVILEGED THREAD mode. It does NOT dispatch (that would run in handler */
/* mode, where a blocking switch could only be deferred, breaking the         */
/* synchronous-blocking contract). The hardware exception frame already holds */
/* the args (r0=nr, r1=a0, r2=a1, r3=a2, r12=a3) and, in LR, the return       */
/* address of arch_syscall's caller -- so we only rewrite the stacked PC.     */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp                 /* r0 -> user hardware frame */
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
/* inline when the thread is next scheduled -- exactly the sim's synchronous   */
/* call semantics. Privilege is dropped with a plain CONTROL write (no trap    */
/* needed to LOWER privilege in thread mode) before returning to the user.     */
    .thumb_func
    .global svc_trampoline
svc_trampoline:
    push    {r12, lr}               /* [sp]=a3 (5th arg), [sp,#4]=caller-return */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0 */
    ldr     lr, [sp, #4]            /* restore caller-return */
    add     sp, sp, #8
    /* Restore the caller's RESTING privilege (not a hard-coded nPRIV=1): a
       privileged thread issuing a syscall must not be demoted. g_arch_current
       is this thread again here (PendSV restores it across any block). */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]                /* current ctx* */
    ldr     r2, [r2, #8]            /* ctx.resting_npriv (offset 8) */
    mrs     r1, control
    bfi     r1, r2, #0, #1          /* CONTROL.nPRIV = resting privilege */
    msr     control, r1
    isb
    bx      lr                      /* -> arch_syscall's caller, r0=result */

/* ------------------------------------------------------------------------- */
/* arch_start(boot, first) -- enter the first thread from boot (thread/MSP).  */
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
/* arch_syscall(nr, a0, a1, a2, a3) -- userspace trap (runs unprivileged).    */
/* AAPCS: r0=nr r1=a0 r2=a1 r3=a2, [sp]=a3. Move a3 into r12 so the exception  */
/* frame carries it; the result is delivered in r0 by svc_trampoline, which    */
/* returns straight to this function's caller (the trailing bx is a fallback). */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_syscall
arch_syscall:
    ldr     r12, [sp]               /* a3 (5th arg) -> stacked r12 */
    svc     #0
    bx      lr

    .end
