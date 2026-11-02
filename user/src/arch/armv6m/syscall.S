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
