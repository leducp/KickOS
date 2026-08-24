/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv6-M (Cortex-M0/M0+) context switch and syscall trap: PendSV switch, SVC redirecting to
 * svc_trampoline in privileged THREAD mode, which relocates SP onto the calling thread's own
 * KERNEL BLOCK (ctx.kernel_sp) and dispatches there, blocking as a synchronous PendSV. The
 * v6-M subset shapes every sequence here:
 *   - PUSH/POP and load/store multiple reach r0-r7, so r8-r11 are shuffled through
 *     r4-r7 (mov hi<->lo) and r12 (a3) travels via a low register;
 *   - CONTROL.nPRIV is set with movs/bics/orrs, addresses come from ldr =literal, and
 *     conditionals are cmp/branch (v6-M has BFI, MOVW and IT in neither encoding);
 *   - thread returns are always 0xFFFFFFFD, rebuilt on switch-in.
 *
 * Saved frame, low->high on PSP:  [r4 r5 r6 r7 r8 r9 r10 r11][hw frame].
 * ctx.sp -> r4. ctx = {sp@0, npriv@4, resting_npriv@8, stack_lo@12, stack_hi@16}.
 *
 * The saved block IS the IPC leaf's argument array, and the v6-M store of it shuffles r8-r11
 * through r4-r7, so a refusal must put r4-r7 back before the generic path resumes on the real
 * register file.
 */

#include <kickos/arch/armv6m_trap_stack.h>
/* KICKOS_KERNEL_STACK_SIZE, which PendSV_Handler compares a PSP against. Pure integer
   macros, generated, and already included from startup.S. */
#include <kickos/board_config.h>

    .syntax unified
    .thumb
    .text
    .cpu cortex-m0plus

/* struct arch_context field offsets the PSP bounds guard reads as plain displacements.
   arch_armv6m.cc static_asserts offsetof against the same macros. */
    .equ F_CTX_STACK_LO, KICKOS_ARMV6M_CTX_OFF_STACK_LO
    .equ F_CTX_STACK_HI, KICKOS_ARMV6M_CTX_OFF_STACK_HI
    .equ F_CTX_KERNEL_SP, KICKOS_ARMV6M_CTX_OFF_KERNEL_SP
    .equ F_CTX_TRACE_TID, KICKOS_ARMV6M_CTX_OFF_TRACE_TID

/* The deepest a PSP may sit inside a kernel block and still be accepted by PendSV's block leg,
   as a distance DOWN from the block's top because that is what the compare there holds. SIZE,
   less the lowest word (the overflow canary in kernel/thread/thread.cc, which no frame may
   consume), less NEST_SVCK: the room the block's own sizing reserves BELOW a dispatch for
   every frame an exception can pile on it, the PendSV save under test among them.
   AS TIGHT AS THE SIZING AND NO TIGHTER: this bound less the deepest legitimate PSP
   (KERNEL_DEPTH_SVCK below the top) is (SIZE - 4) - NEED_SVCK, the block's own slack, which on
   this arch is ZERO. So the compare must accept the equal case or the deepest legal dispatch
   is refused. Same derivation as rxv3's BLOCK_USP_MAX_DROP. */
    .equ BLOCK_PSP_MAX_DROP, \
        KICKOS_KERNEL_STACK_SIZE - KICKOS_ARMV6M_TRAP_NEST_SVCK - 4

/* The software block both guarded pushes write below the live PSP: {r4-r11}, eight
   words. A REGISTER COUNT, not a measured depth, and arch_armv6m.cc asserts it against
   the register list it prices. */
    .equ F_TRAP_FRAME, KICKOS_ARMV6M_TRAP_FRAME

/* PSP bounds guard, used by both trap sites in this file.
   In:  \need = the room this site's class needs below the PSP;
        r0 = the live PSP, r3 = the running thread's arch_context*.
   Clobbers r1 and r2, both already in the hardware frame and so free here. Leaves \need
   in r2, which .Lpsp_wild reports.
   Falls through only when \need bytes fit between stack_lo and the PSP.

   \need is the class figure armv6m_trap_stack.h derives. The two sites pass DIFFERENT ones
   because the SVC site's exception return leaves handler mode, so svc_trampoline keeps
   descending on this PSP until it reaches ctx.kernel_sp. The figure comes out of the literal
   pool: a v6-M movs carries an imm8, and a figure can outgrow it.

   NO ALIGNMENT LEG. CCR.STKALIGN is Read-As-One on ARMv6-M, so exception entry clears bit 2
   of the banked SP before stacking and r0 here is an 8-byte-aligned frame base whatever the
   thread wrote to SP.

   \bad is the refusal target, and the two sites pass DIFFERENT ones because the contained
   resume below reuses PendSV's restore half, whose telemetry tail pops a spill only the
   PendSV entry makes. */
    .macro PSP_GUARD need, bad
    ldr     r2, =\need
    ldr     r1, [r3, #F_CTX_STACK_LO]
    cmp     r0, r1
    blo     9f
    subs    r1, r0, r1              /* the room REMAINING below the PSP: a bound written
                                       as psp - need could wrap under stack_lo instead */
    cmp     r1, r2
    blo     9f
    ldr     r1, [r3, #F_CTX_STACK_HI]
    cmp     r0, r1
    blo     8f                      /* a PSP above the stack would push into whatever
                                       granted region carried the hardware frame */
9:  b       \bad
8:
    .endm

/* ------------------------------------------------------------------------- */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    ldr     r2, =g_arch_current
    ldr     r3, [r2]                /* outgoing ctx* (0 => first start) */
#if KICKOS_TELEMETRY
    /* The tail emits from this spill and the incoming ctx* it loads, never from a
       re-read of g_arch_next. r2 keeps MSP 8-byte aligned across the later bl. */
    push    {r2, r3}
#endif
    cmp     r3, #0
    beq     1f
    /* --- save outgoing --- */
    mrs     r0, psp
    /* THE PSP IS EITHER STACK, and which one is not a thing to refuse. A thread preempted
       mid-dispatch is running on its own kernel block (svc_trampoline), so its PSP is inside
       [kernel_sp - KICKOS_KERNEL_STACK_SIZE, kernel_sp) and the user-stack bounds below would
       refuse every blocking syscall and kill the thread. The block leg is narrowest on
       purpose: THIS thread's kernel_sp and no other, never the whole array. An unseated block
       reads 0, so the first compare fails and the user-stack legs decide, which is what idle
       takes. An aimed PSP forges no EXC_RETURN either: this arch's epilogue rebuilds it from
       a literal.

       THE ROOM IS PART OF THE RANGE, AND THIS IS THE BOARD THAT PROVES IT. A bare range test
       admits a PSP at the block's base, and the save below then writes THROUGH THE CANARY and
       into the neighbouring slot's privileged continuation. "Every byte below a kernel PSP is
       privileged code's" holds only where the thread cannot aim its own PSP at the block; this
       arch selects ARCH_KERNEL_STACKS_MANDATORY, so microbit carves blocks with NO MPU and the
       thread can. The bound stops BLOCK_PSP_MAX_DROP short of the base. */
    ldr     r1, [r3, #F_CTX_KERNEL_SP]
    subs    r1, r1, r0              /* bytes from the PSP up to the block's top */
    bls     2f                      /* at or above the top, so not on the block */
    ldr     r2, =BLOCK_PSP_MAX_DROP
    cmp     r1, r2
    bls     3f                      /* on this thread's own block, a whole nest still below.
                                       Accepts the EQUAL case: this arch's block has no slack,
                                       so the deepest legal dispatch sits ON the bound. */
2:
    PSP_GUARD KICKOS_ARMV6M_TRAP_NEED_PENDSV, .Lpsp_wild
3:
    subs    r0, #F_TRAP_FRAME       /* room for r4-r11 */
    str     r0, [r3]                /* outgoing->sp = base */
    stmia   r0!, {r4-r7}            /* [base..+12] = r4-r7 */
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    stmia   r0!, {r4-r7}            /* [base+16..+28] = r8-r11 */
    mrs     r0, control             /* save current privilege */
    movs    r1, #1
    ands    r0, r1
    str     r0, [r3, #4]            /* outgoing->npriv */
1:
/* Also the CONTAINED resume: every word below comes from a literal or from the incoming
   context, so nothing the skipped save half would have left in a register is read here. */
.Lrestore_incoming:
    /* g_arch_next and the deferred MPU stash are ONE pair, written together under the
       kernel lock, and PendSV must consume them as one. A device IRQ preempting between
       the two (every v6-M device line sits at reset priority 0, above PendSV)
       reschedules and replaces BOTH, so an unmasked PendSV can restore one thread's PSP
       and then program the other thread's regions, and the resumed thread faults on its
       own stack. PRIMASK masks PendSV itself, so entry here implies PRIMASK==0 and the
       cpsie below restores the entry state exactly. */
    cpsid   i
    ldr     r2, =g_arch_current
    ldr     r1, =g_arch_next
    ldr     r1, [r1]                /* incoming ctx* */
    str     r1, [r2]                /* g_arch_current = incoming */
    ldr     r0, [r1]                /* base */
    adds    r0, #16
    ldmia   r0!, {r4-r7}            /* r8-r11 values -> r4-r7, r0 -> base+32 */
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    msr     psp, r0                 /* PSP = base+32 = hw frame */
    ldr     r0, [r1]                /* reload base */
    ldmia   r0!, {r4-r7}            /* restore r4-r7 */
    ldr     r0, [r1, #4]            /* incoming npriv */
    mrs     r2, control
    movs    r3, #1
    bics    r2, r3                  /* clear nPRIV */
    orrs    r2, r0                  /* set nPRIV */
    msr     control, r2
    isb
#if KICKOS_TELEMETRY
    /* SWITCH completion hook. lr is clobbered by the bl and rebuilt (0xFFFFFFFD) right
       after, so it needs no save. from = 0xFFFF on the first switch. */
    pop     {r2, r3}                /* r3 = outgoing ctx* (r2 = alignment pad) */
    ldr     r1, [r1, #F_CTX_TRACE_TID] /* r1 = to_tid (incoming.trace_tid) */
    ldr     r0, =0xFFFF             /* default from = no-thread */
    cmp     r3, #0
    beq     4f
    ldr     r0, [r3, #F_CTX_TRACE_TID] /* from_tid = outgoing.trace_tid */
4:
    bl      kickos_trace_switch_done /* (r0=from_tid, r1=to_tid) */
#endif
    /* Deferred MPU commit: must run after the physical PSP/register swap (shared seam,
       arch_arm_common.cc). r4-r11 are callee-saved across the call, r0-r3/r12 are
       reloaded from the HW frame on exception return, and lr is rebuilt right below. */
    bl      kickos_arch_mpu_commit
    cpsie   i                       /* pair consumed; never resume a thread masked */
    ldr     r0, =0xFFFFFFFD         /* EXC_RETURN: thread mode, PSP */
    mov     lr, r0
    bx      lr

/* A syscall with no current thread has no bounds to check, and the generic path below this
   handler still writes through the PSP. Unreachable today: g_arch_current is the first-start
   sentinel only until arch_start's first switch, and nothing traps before then. The bounds go
   out as [0, 0), which the reporter's own upper-bound test describes as an unseated pair.
   Never returns, so r4 is free to carry the target: a v6-M ldr literal cannot reach r12, and
   a plain b is a +-2 KB reach that --gc-sections can outrun. */
.Lsvc_nothread:
    ldr     r1, =KICKOS_ARMV6M_TRAP_NEED_SVC
    movs    r2, #0
    movs    r3, #0
    ldr     r4, =kickos_armv6m_bad_psp
    bx      r4                      /* (r0=PSP, r1=need, r2=0, r3=0) */

/* The SVC entry alone. r3 is still the outgoing ctx*, which is exactly what the shared tail
   expects to pop, so the spill the PendSV entry made is faked here rather than the pop being
   skipped: a contained refusal IS a physical swap and must emit its switch record, or the
   decoder's from == previous.to continuity check breaks on the next one. r2 is the pad. */
.Lpsp_wild_svc:
#if KICKOS_TELEMETRY
    push    {r2, r3}
#endif

/* The PSP has no room for the software block. stack_lo is read before r3 is overwritten with
   stack_hi, and r4 carries the target for the reason above. */
.Lpsp_wild:
    mov     r1, r2                  /* the figure THIS site asked for, which the guard left
                                       in r2. A literal here would report the switcher's 32
                                       for an SVC refusal that charged the whole extent. */
    ldr     r2, [r3, #F_CTX_STACK_LO]
    ldr     r3, [r3, #F_CTX_STACK_HI]
    ldr     r4, =kickos_armv6m_bad_psp
    blx     r4                      /* (r0=PSP, r1=need, r2=stack_lo, r3=stack_hi) */
    /* Returns only when the offending thread was contained: it is slain and its next resume
       is claimed, and g_arch_next is the thread to run instead. */
    b       .Lrestore_incoming

/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp
    /* Guards BOTH exits from here, at the SVC figure, which is the larger of the two: the
       fastpath below pushes the callee block through this PSP and returns in handler mode,
       while the slow path exception-returns into svc_trampoline, which runs PRIVILEGED IN
       THREAD MODE on this same PSP until it reaches ctx.kernel_sp. The guard sits above the
       `cmp #56` that picks the arm, so it charges whichever spends more. */
    ldr     r3, =g_arch_current
    ldr     r3, [r3]
    cmp     r3, #0
    beq     .Lsvc_nothread          /* out of line below, and it does not return */
    PSP_GUARD KICKOS_ARMV6M_TRAP_NEED_SVC, .Lpsp_wild_svc
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
       here, with an uncommitted region stash behind it. SVCall tail-chains ahead of PendSV,
       so entry in that state is reachable. */
    mrs     r1, primask
    cmp     r1, #0
    bne     .Lsvc_slow
    ldr     r1, =0xE000ED04         /* SCB->ICSR */
    ldr     r1, [r1]
    ldr     r2, =0x10000000         /* PENDSVSET */
    tst     r1, r2
    bne     .Lsvc_slow
    /* The kernel lock IS PRIMASK on v6-M, so masking at the one level there is nests
       exactly: each IrqLock inside the leaf saves 1, masks, and restores 1. */
    cpsid   i
    /* Save the callee block exactly as PendSV does. That block IS the leaf's argument
       array: args[0] = r4 (the result slot the resuming switch writes), args[1] = r5
       (endpoint cap), args[2] = r6 (packed lengths), args[3..7] = r7-r11 (the payload).
       PSP is not moved, so a refusal below leaves the words as dead scratch under the
       thread's own stack pointer. */
    subs    r0, #F_TRAP_FRAME
    stmia   r0!, {r4-r7}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    stmia   r0!, {r4-r7}
    subs    r0, #F_TRAP_FRAME       /* r0 = the block base = args */
    push    {r0, lr}                /* the frame base and EXC_RETURN, across the bl */
    bl      kickos_ipc_fastpath
    pop     {r1, r2}
    mov     lr, r2                  /* v6-M pop cannot target LR */
    cmp     r0, #0
    beq     .Lsvc_fp_refused
    /* Taken. r0 = the incoming thread's arch_context*, r1 = the outgoing block base. The
       stacked PC already points past the SVC, so the caller's resume is a plain exception
       return and the switch that resumes it seats the result (sched.cc switch_book).
       The physical swap and the region commit are ONE pair: an interrupt between them
       would resume a thread under another thread's regions. PRIMASK was 0 at entry, so
       the cpsie below restores it. */
    ldr     r2, =g_arch_current
    ldr     r3, [r2]                /* outgoing ctx* (the caller) */
    str     r1, [r3]                /* outgoing->sp = this block */
    str     r0, [r2]                /* g_arch_current = incoming */
    mrs     r1, control
    movs    r2, #1
    ands    r1, r2                  /* the caller's own nPRIV: this path never raised it */
    str     r1, [r3, #4]
#if KICKOS_TELEMETRY
    push    {r2, r3}                /* outgoing ctx*, across the restore below */
#endif
    ldr     r1, [r0]                /* incoming block base */
    adds    r1, #16
    ldmia   r1!, {r4-r7}            /* r8-r11 values -> r4-r7, r1 -> base+32 */
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    msr     psp, r1                 /* PSP = base+32 = the hw frame */
    ldr     r1, [r0]                /* reload base */
    ldmia   r1!, {r4-r7}
    ldr     r1, [r0, #4]            /* incoming npriv */
    mrs     r2, control
    movs    r3, #1
    bics    r2, r3
    orrs    r2, r1
    msr     control, r2
    isb
#if KICKOS_TELEMETRY
    /* SWITCH completion hook: a physically swapped pair that emitted nothing would break
       the decoder's from == previous.to continuity check. The outgoing ctx* is always
       live here, so from_tid is read unconditionally. lr is clobbered by the bl and
       rebuilt below. */
    pop     {r2, r3}                /* r3 = outgoing ctx* */
    ldr     r1, [r0, #F_CTX_TRACE_TID] /* to_tid (incoming.trace_tid) */
    ldr     r0, [r3, #F_CTX_TRACE_TID] /* from_tid */
    bl      kickos_trace_switch_done
#endif
    /* Deferred MPU commit for the incoming thread: the physical swap is done and the
       mask is still up. */
    bl      kickos_arch_mpu_commit
    cpsie   i                       /* the pair is consumed */
    ldr     r0, =0xFFFFFFFD         /* EXC_RETURN: thread mode, PSP */
    mov     lr, r0
    bx      lr
.Lsvc_fp_refused:
    /* The block store above shuffled r8-r11 through r4-r7, so the caller's r4-r7 survive
       only in the dead words below PSP. The generic path resumes on the register file. */
    ldmia   r1!, {r4-r7}
    cpsie   i
    mrs     r0, psp                 /* PSP never moved; the redirect below wants it back */
.Lsvc_slow:
#endif
    ldr     r1, =svc_trampoline
    movs    r2, #1
    bics    r1, r2                  /* stacked PC must be halfword-aligned (bit0=0;
                                       the T-bit comes from the stacked xPSR) */
    str     r1, [r0, #24]           /* frame.PC := svc_trampoline */
    mrs     r1, control
    movs    r2, #1
    bics    r1, r2                  /* CONTROL.nPRIV = 0 (privileged) */
    msr     control, r1
    isb
    bx      lr                      /* -> svc_trampoline: r0=nr r1=a0 r2=a1
                                       r3=a2 r12=a3 lr=caller-return */

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
    push    {r0, r1}                /* the two scratch registers the loads below need */
    ldr     r0, =g_arch_current
    ldr     r0, [r0]
    ldr     r0, [r0, #F_CTX_KERNEL_SP]
    cmp     r0, #0
    beq     .Lsvc_nokstack          /* no block to build on, and it does not return */
    mov     r1, sp
    adds    r1, #8                  /* the PSP the thread resumes on: the push undone */
    subs    r0, #8
    str     r1, [r0]
    mov     r1, lr
    subs    r0, #4
    str     r1, [r0]
    mov     r1, r12
    subs    r0, #4
    str     r1, [r0]                /* a3 into the fifth-argument slot */
    mov     r12, r0                 /* r12 is free once a3 is in the header */
    pop     {r0, r1}
    mov     sp, r12                 /* on the calling thread's kernel block from here */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0:r1 */
    ldr     r2, [sp, #4]            /* the caller-return */
    mov     lr, r2
    ldr     r2, [sp, #8]            /* the resume PSP, read before sp leaves this block */
    mov     sp, r2
    /* Drop to the caller's resting privilege using r2/r3 ONLY: r0:r1 carry the
       AAPCS long long result out of this trampoline. That leaves no register to
       hold the #1 mask, so bit 0 is cleared by a shift pair instead of bics. */
    ldr     r2, =g_arch_current
    ldr     r2, [r2]
    ldr     r2, [r2, #8]            /* ctx.resting_npriv */
    mrs     r3, control
    lsrs    r3, r3, #1
    lsls    r3, r3, #1              /* CONTROL.nPRIV = 0 */
    orrs    r3, r2                  /* CONTROL.nPRIV = resting privilege */
    msr     control, r3
    isb
    bx      lr                      /* -> arch_syscall's caller, r0:r1=result */

/* A thread whose ctx.kernel_sp is 0 reached the syscall path, so the transfer has nowhere to
   build. Unreachable today: thread_create seats a block on every pool thread, and idle is the
   only TCB outside the pool, privileged and running arch_idle_wait alone. Reported rather
   than argued, so a future TCB that reaches it finds a refusal instead of a store through
   0xFFFFFFF0. Reached through a literal for the reason .Lpsp_wild is. */
.Lsvc_nokstack:
    /* PRIMASK IS SET BEFORE SPSEL MOVES, and the order is load-bearing. The exception
       return into svc_trampoline dropped below PRIO_SVCALL, so PendSV is live here. Its
       save assumes the interrupted thread was on that PSP, and it exception-returns on a
       hardcoded 0xFFFFFFFD, which selects the PSP whatever stack the entry frame went to.
       Taken after SPSEL is 0 the hardware stacked that frame on the MSP, so the resume pops
       a frame from the wrong bank. kpanic_enter masks too, one call later, which is one
       call too late. Noreturn, so no cpsie pairs. */
    cpsid   i
    mov     r0, sp
    adds    r0, #8                  /* the PSP the thread would have resumed on */
    /* THE REPORTER RUNS ON THE MSP. This site is in THREAD mode on the thread's own PSP, and
       the reporter's console chain measures 536 bytes against the 40 the SVC guard reserved
       below that PSP, so reporting on the PSP overflows the thread's stack while printing.
       SPSEL is cleared after r0 is computed, so the reported PSP is the thread's; the
       push {r0, r1} above is abandoned with it, the reporter being noreturn. v6-M has no
       immediate bic, so the mask comes from a register. */
    mrs     r2, control
    movs    r3, #2
    bics    r2, r3                  /* CONTROL.SPSEL = 0: SP is the MSP from here */
    msr     control, r2
    isb
    ldr     r1, =kickos_armv6m_no_kernel_stack
    bx      r1

/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_start
arch_start:                         /* r0=boot (ignored), r1=first */
    ldr     r2, =g_arch_current
    movs    r3, #0
    str     r3, [r2]                /* first-start sentinel */
    ldr     r2, =g_arch_next
    str     r1, [r2]
    ldr     r2, =0xE000ED04         /* SCB->ICSR */
    ldr     r3, =0x10000000         /* PENDSVSET */
    str     r3, [r2]
    dsb
    isb
    bx      lr

/* ------------------------------------------------------------------------- */
/* arch_syscall(nr, a0, a1, a2, a3): r0=nr r1=a0 r2=a1 r3=a2 [sp]=a3.          */
/* Move a3 into r12 so the exception frame carries it (r12 can't be ldr'd on   */
/* v6-M -> via a low reg). Result delivered in r0:r1 by svc_trampoline.        */
/* arch_syscall64 is the SAME trap; r0:r1 is the AAPCS long long pair.         */
    .thumb_func
    .global arch_syscall64
arch_syscall64:
    .thumb_func
    .global arch_syscall
arch_syscall:
    push    {r4}                    /* free a low reg (shifts [sp] a3 by +4) */
    ldr     r4, [sp, #4]            /* a3 */
    mov     r12, r4
    pop     {r4}                    /* restore r4 + SP (8B aligned again) */
    svc     #0
    bx      lr

#if KICKOS_ARCH_HAS_IPC_FASTPATH
/* ------------------------------------------------------------------------- */
/* int32_t arch_syscall_reg(uint32_t* io): the register-carrying IPC trap.    */
/* The hardware frame holds only five words, so the payload rides the CALLEE  */
/* block instead: io[1..7] load r5-r11, which SVC_Handler stores contiguously */
/* below the frame for the kernel leaf to read and write. r0 keeps the number */
/* so a refusal reaching syscall_dispatch still answers on it; r4 is left     */
/* alone because it is the result slot. On return io[1..5] take back r5-r9.   */
/* v6-M reaches r8-r11 only through low registers, so both the save and the   */
/* payload load go via r2/r3 and the reply store comes back the same way.     */
/* ------------------------------------------------------------------------- */
    .thumb_func
    .global arch_syscall_reg
arch_syscall_reg:
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r0, r4-r7}             /* [sp] = io, then the caller's r8-r11 */
    adds    r1, r0, #4              /* one walk up io: the 3-in-1 T1 add reaches #4, not #16 */
    ldmia   r1!, {r5, r6, r7}       /* io[1], io[2], io[3] */
    ldmia   r1!, {r2, r3}           /* io[4], io[5] */
    mov     r8, r2
    mov     r9, r3
    ldmia   r1!, {r2, r3}           /* io[6], io[7] */
    mov     r10, r2
    mov     r11, r3
    ldr     r0, [r0]                /* nr */
    /* The two outcomes land at two different addresses, and nothing may run between the
       SVC and the declined one: svc_trampoline exits to the STACKED LR, so the declined
       arm resumes at whatever LR holds here. The taken path restores the hardware frame
       and resumes at the SVC's own return address. */
    ldr     r1, =.Lreg_declined
    adds    r1, #1                  /* the trampoline exits with bx: Thumb bit required */
    mov     lr, r1
    svc     #0
    ldr     r1, [sp]                /* taken: r4 = result, r5-r9 = the reply */
    adds    r1, #4
    stmia   r1!, {r5, r6, r7}
    mov     r5, r8
    mov     r6, r9
    stmia   r1!, {r5, r6}
    mov     r0, r4
.Lreg_declined:                     /* r0 = the generic dispatch's KOS_CALL_REG_FALLBACK */
    pop     {r1, r4-r7}             /* r1 discards io; r4-r7 carry the caller's r8-r11 */
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7}
    pop     {r1}
    bx      r1
#endif

    .end
