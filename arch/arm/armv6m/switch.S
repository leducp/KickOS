/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ARMv6-M (Cortex-M0/M0+) context switch + syscall trap. Same design as armv7m
 * (PendSV switch; SVC redirects to svc_trampoline running in privileged THREAD
 * mode; blocking = synchronous PendSV) but rewritten for the v6-M subset:
 *   - PUSH/POP and load/store multiple only reach r0-r7 -> r8-r11 are shuffled
 *     through r4-r7 (mov hi<->lo), and r12 (a3) is moved via a low register;
 *   - no BFI/MOVW/IT; CONTROL.nPRIV is set with movs/bics/orrs; addresses via
 *     ldr =literal; conditionals via cmp/branch;
 *   - no FPU, so no s-register save and no EXC_RETURN stored in the frame
 *     (thread returns are always 0xFFFFFFFD, reconstructed on switch-in).
 *
 * Saved frame, low->high on PSP:  [r4 r5 r6 r7 r8 r9 r10 r11][hw frame].
 * ctx.sp -> r4. ctx = {sp@0, npriv@4, resting_npriv@8}.
 */

    .syntax unified
    .thumb
    .text
    .cpu cortex-m0plus

/* ------------------------------------------------------------------------- */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    ldr     r2, =g_arch_current
    ldr     r3, [r2]                /* outgoing ctx* (0 => first start) */
#if KICKOS_TELEMETRY
    /* Spill the outgoing ctx* before the swap clobbers it; the tail emits from
       this + the incoming ctx* it loads, never re-reading g_arch_next. r2 is
       pushed only to keep MSP 8-byte aligned across the later bl. */
    push    {r2, r3}
#endif
    cmp     r3, #0
    beq     1f
    /* --- save outgoing --- */
    mrs     r0, psp
    subs    r0, #32                 /* room for r4-r11 */
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
    /* SWITCH completion hook, BEFORE the EXC_RETURN reconstruction: lr is
       clobbered by the bl but rebuilt (0xFFFFFFFD) right after, so it needs no
       save. r1 = incoming ctx* (loaded above); the popped r3 = outgoing ctx*.
       trace_tid @ offset 12; from = 0xFFFF on the first switch. */
    pop     {r2, r3}                /* r3 = outgoing ctx* (r2 = alignment pad) */
    ldr     r1, [r1, #12]           /* r1 = to_tid (incoming.trace_tid) */
    ldr     r0, =0xFFFF             /* default from = no-thread */
    cmp     r3, #0
    beq     4f
    ldr     r0, [r3, #12]           /* from_tid = outgoing.trace_tid */
4:
    bl      kickos_trace_switch_done /* (r0=from_tid, r1=to_tid) */
#endif
    ldr     r0, =0xFFFFFFFD         /* EXC_RETURN: thread mode, PSP */
    mov     lr, r0
    bx      lr

/* ------------------------------------------------------------------------- */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    mrs     r0, psp
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

    .thumb_func
    .global svc_trampoline
svc_trampoline:
    push    {r4, lr}                /* [sp]=user r4, [sp+4]=caller-return (8B) */
    mov     r4, r12                 /* a3 */
    sub     sp, #8                  /* 5th-arg slot (keep 8B alignment) */
    str     r4, [sp]                /* [sp] = a3 */
    bl      syscall_dispatch        /* (r0..r3, [sp]=a3) -> result in r0 */
    add     sp, #8                  /* r0 = result from here on */
    /* Drop to the caller's resting privilege while r1-r3 are free (r0 kept). */
    ldr     r1, =g_arch_current
    ldr     r1, [r1]
    ldr     r1, [r1, #8]            /* ctx.resting_npriv */
    mrs     r2, control
    movs    r3, #1
    bics    r2, r3
    orrs    r2, r1                  /* CONTROL.nPRIV = resting privilege */
    msr     control, r2
    isb
    /* Restore user r4 and the caller-return (v6-M pop can't target LR, so pop
       the return address into a low reg and branch to it). */
    pop     {r4}                    /* user r4 */
    pop     {r1}                    /* caller-return */
    bx      r1                      /* -> arch_syscall's caller, r0=result */

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
/* v6-M -> via a low reg). Result delivered in r0 by svc_trampoline.           */
    .thumb_func
    .global arch_syscall
arch_syscall:
    push    {r4}                    /* free a low reg (shifts [sp] a3 by +4) */
    ldr     r4, [sp, #4]            /* a3 */
    mov     r12, r4
    pop     {r4}                    /* restore r4 + SP (8B aligned again) */
    svc     #0
    bx      lr

    .end
