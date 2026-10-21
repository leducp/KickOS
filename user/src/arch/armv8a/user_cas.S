/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .section .text.kickos_user_cas, "ax", %progbits

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired) */
    .global kickos_user_cas
    .type kickos_user_cas, %function
    .balign 4
kickos_user_cas:
1:  ldaxr   w3, [x0]
    cmp     w3, w1
    b.ne    2f
    stlxr   w4, w2, [x0]
    cbnz    w4, 1b
    mov     w0, w3
    ret
2:  clrex
    mov     w0, w3
    ret
    .size kickos_user_cas, . - kickos_user_cas
