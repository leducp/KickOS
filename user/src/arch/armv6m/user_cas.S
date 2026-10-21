/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * PendSV resumes a thread switched out in [kickos_user_cas, kickos_user_cas_store] at
 * kickos_user_cas: nothing in that window may overwrite its own input.
 */

    .syntax unified
    .thumb
    .cpu cortex-m0plus

    .section .text.kickos_user_cas, "ax", %progbits

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired) */
    .thumb_func
    .global kickos_user_cas
    .type kickos_user_cas, %function
kickos_user_cas:
    ldr     r3, [r0]
    cmp     r3, r1
    bne     1f
    .global kickos_user_cas_store
kickos_user_cas_store:
    str     r2, [r0]
1:  movs    r0, r3
    bx      lr
    .size kickos_user_cas, . - kickos_user_cas
