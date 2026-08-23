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
 *     [r4 r5 r6 r7 r8 r9 r10 r11 EXC_RETURN]   <- PendSV-saved (ctx.sp points here)
 *     [r0 r1 r2 r3 r12 LR PC xPSR]             <- hardware exception frame
 * On an FPU part a thread with live FP context also carries {s16-s31} between the callee
 * block and the hardware frame (saved before {r4-r11,lr}, so at higher addresses) and
 * {s0-s15,FPSCR} inside the hardware frame; both are handled conditionally on EXC_RETURN
 * bit4.
 */

#include <kickos/arch/armv7m_trap_stack.h>
/* KICKOS_KERNEL_STACK_SIZE, which PendSV_Handler compares a PSP against. Pure integer
   macros, generated, and already included from startup.S. */
#include <kickos/board_config.h>

    .syntax unified
    .thumb

    .equ F_CTX_STACK_LO, KICKOS_ARMV7M_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_ARMV7M_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_ARMV7M_CTX_OFF_KERNEL_SP
    .equ F_CTX_TRACE_TID, KICKOS_ARMV7M_CTX_OFF_TRACE_TID

/* PSP bounds guard, used by both software pushes in this file.
   In:  \need = the room this site's class needs below the PSP, before the FP term;
        r0 = the live PSP, r2 = the running thread's arch_context*, lr = EXC_RETURN.
   Clobbers r1 and r12, both already in the hardware frame and so free here.
   Falls through only when \need bytes, plus {s16-s31} when the FP frame is live, fit
   between stack_lo and the PSP.

   \need is the class figure armv7m_trap_stack.h derives. The two sites pass DIFFERENT ones
   because the SVC site's exception return leaves handler mode, so svc_trampoline keeps
   descending on this PSP until it reaches ctx.kernel_sp.

   \fp charges the {s16-s31} block AT RUN TIME on an FP-live entry, and only PendSV may take
   it: its push IS that block, while the SVC figure is a structural constant that already
   dominates BOTH entry postures, so charging the term there charges it twice and refuses a
   legal floor-sized FP-active thread. */
    .macro PSP_GUARD need, fp
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
/* PendSV_Handler runs at the lowest priority, after every other exception    */
/* tail-chains, so a switch requested from an ISR happens on the way back to  */
/* thread level.                                                              */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
#if KICKOS_BENCH
    /* DWT CYCCNT @ 0xE0001004. r0/r1 are caller-saved and already HW-stacked, so they
       are free here. */
    ldr     r0, =0xE0001004
    ldr     r0, [r0]
    ldr     r1, =g_bench_sw_start
    str     r0, [r1]
#endif
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

       NO ROOM TEST ON THIS LEG. Every byte below a kernel PSP is written by privileged code
       through a pointer the kernel seated, so what has to hold is a SIZE and not a bound;
       armv7m_trap_stack.h's SVCK class prices it. The block's lowest word is its overflow
       canary (kernel/thread/thread.cc). */
    ldr     r1, [r2, #F_CTX_KERNEL_SP]
    subs    r1, r1, r0              /* bytes from the PSP up to the block's top */
    bls     2f                      /* at or above the top, so not on the block */
    movw    r12, #KICKOS_KERNEL_STACK_SIZE
    cmp     r1, r12
    bls     3f                      /* inside this thread's own kernel block */
2:
#endif
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_PENDSV, 1
#if KICKOS_KERNEL_STACKS
3:
#endif
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
    /* Bench: delta = now - entry stamp. Read BEFORE any telemetry emit, so the measured
       window brackets the save/restore alone. The balanced push/pop preserves r1 (incoming
       ctx*) and the entry-spilled {r2,r3} beneath it, keeps MSP 8-byte aligned and saves
       EXC_RETURN. */
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
    /* SWITCH completion hook. Emit {from,to} from the PHYSICALLY-swapped contexts: r1 is
       the incoming ctx* loaded above, r2 the outgoing ctx* spilled at entry. from = 0xFFFF
       on the first switch, where the outgoing ctx* is 0. EXC_RETURN (lr) is saved across
       the bl and MSP kept 8-byte aligned. */
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
    /* Deferred MPU commit, after the physical PSP/register swap (shared seam,
       arch_arm_common.cc). Applying the incoming regions in switch_to instead would run the
       OUTGOING thread under them until PendSV fires, faulting it on its own stack. Callee
       regs (r4-r11) survive the call (AAPCS), r0-r3/r12 are reloaded from the HW frame on
       exception return, and lr holds EXC_RETURN and must be preserved across the bl (the r0
       pad keeps MSP 8-byte aligned). */
    push    {r0, lr}
    bl      kickos_arch_mpu_commit
    pop     {r0, lr}
    cpsie   i                       /* pair consumed; never resume a thread masked */
    bx      lr                      /* EXC_RETURN -> resume incoming on PSP */

/* A syscall with no current thread has no bounds to check, and the slow path below this
   handler still writes through the PSP. Unreachable today: g_arch_current is the first-start
   sentinel only until arch_start's first switch, and nothing traps before then. The bounds go
   out as [0, 0), which the reporter's own upper-bound test describes as an unseated pair.
   Never returns. */
.Lsvc_nothread:
    movw    r1, #KICKOS_ARMV7M_TRAP_NEED_SVC
    movs    r2, #0
    movs    r3, #0
    b       kickos_armv7m_bad_psp   /* (r0=PSP, r1=need, r2=0, r3=0) */

/* The PSP has no room for the software block. stack_hi is read into r3 FIRST because r2 is
   the ctx* both loads read. Never returns. */
.Lpsp_wild:
    ldr     r3, [r2, #F_CTX_STACK_HI]
    ldr     r2, [r2, #F_CTX_STACK_LO]
    b       kickos_armv7m_bad_psp   /* (r0=PSP, r1=need, r2=stack_lo, r3=stack_hi) */

/* ------------------------------------------------------------------------- */
/* SVC_Handler redirects the exception return into svc_trampoline, which runs */
/* in PRIVILEGED THREAD mode and dispatches from there, so a blocking switch  */
/* is a real synchronous one (arch.h arch_syscall contract). The hardware     */
/* frame already holds the args (r0=nr r1=a0 r2=a1 r3=a2 r12=a3) and, in LR,  */
/* the return address of arch_syscall's caller, so only the stacked PC is     */
/* rewritten.                                                                 */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp                 /* r0 -> user hardware frame */
    /* Guards BOTH exits from here, at the SVC figure, which is the larger of the two: the
       fastpath below pushes the callee block through this PSP and returns in handler mode,
       while the slow path exception-returns into svc_trampoline, which runs PRIVILEGED IN
       THREAD MODE on this same PSP until it reaches ctx.kernel_sp. The guard sits above the
       `cmp #56` that picks the arm, so it charges whichever spends more. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]
    /* Inverted so the far branch is a plain b: cbz reaches only +126 bytes and the refusal
       is out of line. */
    cbnz    r2, 1f
    b       .Lsvc_nothread          /* no thread running, and it does not return */
1:
    PSP_GUARD KICKOS_ARMV7M_TRAP_NEED_SVC, 0
#if KICKOS_ARCH_HAS_IPC_FASTPATH
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
    stmdb   r0!, {r4-r11, lr}
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

/* THE TRANSFER. Entered privileged in THREAD mode by the SVC's own exception return, with */
/* the hardware frame already popped, so sp is the PSP the calling thread will resume on   */
/* and the frame size and its STKALIGN pad are the hardware's arithmetic and not this      */
/* code's. It relocates sp onto that thread's own kernel block and calls syscall_dispatch  */
/* there. The only bytes spent on the thread's stack are the eight the push below writes,  */
/* and they sit inside the frame the return just popped.                                   */
/*                                                                                         */
/* THE CONTINUATION HEADER, laid at the block's top T and read back after the call:        */
/*     [T-16]  a3, the AAPCS fifth argument, which syscall_dispatch reads at [sp]          */
/*     [T-12]  the caller-return this exits to                                             */
/*     [T-8]   the PSP the thread resumes on                                               */
/*     [T-4]   the word that keeps sp 8-byte aligned for the call                          */
/* The resume PSP lives HERE and not in a register or a second context field: a blocking   */
/* dispatch has PendSV freeze its continuation BELOW this header, so the header survives   */
/* the block for free, and ctx.sp names whichever stack the thread was preempted on.       */
/*                                                                                         */
/* The drop to the caller's resting privilege comes AFTER sp is back on the thread's own   */
/* stack: the header is kernel .bss and an unprivileged read of it faults.                 */
    .thumb_func
    .global svc_trampoline
svc_trampoline:
#if KICKOS_KERNEL_STACKS
    push    {r0, r1}                /* the two scratch registers the loads below need */
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    ldr     r0, [r0, #F_CTX_KERNEL_SP]
    /* Inverted so the far branch is a plain b: cbz reaches only +126 bytes and the refusal
       is out of line. */
    cbnz    r0, 1f
    b       .Lsvc_nokstack          /* no block to build on, and it does not return */
1:
    add     r1, sp, #8              /* the PSP the thread resumes on: the push undone */
    str     r1, [r0, #-8]
    str     lr, [r0, #-12]
    str     r12, [r0, #-16]         /* a3 into the fifth-argument slot */
    sub     r12, r0, #16            /* r12 is free once a3 is in the header */
    pop     {r0, r1}
    mov     sp, r12                 /* on the calling thread's kernel block from here */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     lr, [sp, #4]            /* the caller-return */
    ldr     r2, [sp, #8]            /* the resume PSP, read before sp leaves this block */
    mov     sp, r2
    /* Restore the caller's RESTING privilege, so a privileged thread issuing a syscall keeps
       its posture. g_arch_current is this thread again here, PendSV having restored it across
       any block. r2/r3 only: r0:r1 carry the result pair (AAPCS long long) out of this
       trampoline. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]                /* current ctx* */
    ldr     r2, [r2, #8]            /* ctx.resting_npriv (offset 8) */
    mrs     r3, control
    bfi     r3, r2, #0, #1          /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */

/* A thread whose ctx.kernel_sp is 0 reached the syscall path, so the transfer has nowhere to
   build. Unreachable today: thread_create seats a block on every pool thread, and idle is the
   only TCB outside the pool, privileged and running arch_idle_wait alone. Reported rather
   than argued, so a future TCB that reaches it finds a refusal instead of a store through
   0xFFFFFFF0. r0 is the PSP the thread would have resumed on.

   THE REPORTER RUNS ON THE MSP. This site is in THREAD mode on the thread's own PSP, and the
   reporter's console chain measures 488 bytes against the 180 the SVC guard reserved below
   that PSP, so reporting on the PSP overflows the thread's stack while printing. SPSEL is
   written after r0 is computed, so the reported PSP is the thread's; the push {r0, r1} above
   is abandoned with it, the reporter being noreturn.

   PRIMASK IS SET BEFORE SPSEL MOVES, and the order is load-bearing. The exception return into
   svc_trampoline dropped below PRIO_SVCALL, so PendSV is live here, and its save reads r0
   from the PSP and banks the handler's own lr as the outgoing EXC_RETURN, both assuming the
   interrupted thread was ON that PSP. Taken after SPSEL is 0 the hardware stacked the entry
   frame on the MSP instead, so the saved context describes a stack the thread was not running
   on. kpanic_enter masks too, one call later, which is one call too late. Noreturn, so no
   cpsie pairs with it. */
.Lsvc_nokstack:
    cpsid   i
    add     r0, sp, #8
    mrs     r1, control
    bic     r1, r1, #2              /* CONTROL.SPSEL = 0: SP is the MSP from here */
    msr     control, r1
    isb
    b       kickos_armv7m_no_kernel_stack
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

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3), unprivileged. AAPCS: r0=nr r1=a0 r2=a1   */
/* r3=a2, [sp]=a3, so a3 moves to r12 for the exception frame to carry it.    */
/* svc_trampoline delivers the result in r0:r1 and returns straight to this   */
/* function's caller (the trailing bx is a fallback). arch_syscall64 is the   */
/* SAME trap: r0:r1 is the AAPCS long long pair.                              */
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
    /* The two outcomes land at two different addresses, and nothing may run between the SVC
       and the declined one: svc_trampoline exits to the STACKED LR, so the declined arm
       resumes at whatever LR holds here. The taken path restores the hardware frame and
       resumes at the SVC's own return address. */
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
