/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .syntax unified
    .thumb

    .section .text.kickos_user_cas, "ax", %progbits

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired) */
    .thumb_func
    .global kickos_user_cas
    .type kickos_user_cas, %function
kickos_user_cas:
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8_1M_MAIN__)
1:  ldaex   r3, [r0]
    cmp     r3, r1
    bne     2f
    stlex   r12, r2, [r0]
#else
1:  ldrex   r3, [r0]
    cmp     r3, r1
    bne     2f
    strex   r12, r2, [r0]
#endif
    cmp     r12, #0
    bne     1b
    movs    r0, r3
    bx      lr
2:  clrex
    movs    r0, r3
    bx      lr
    .size kickos_user_cas, . - kickos_user_cas
