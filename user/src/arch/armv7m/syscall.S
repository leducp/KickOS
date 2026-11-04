/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The user side of the syscall trap: arch_syscall(nr, a0, a1, a2, a3), arch_syscall64 the same
 * entry, and arch_syscall_reg where the arch carries the register-form IPC trap. Part of the user
 * runtime, so every image carries its own.
 */

    .syntax unified
    .thumb

    .section .text.kickos_arch_syscall, "ax", %progbits
    .balign 4               /* the reg trap's ADR offset assumes a word-aligned section */

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
