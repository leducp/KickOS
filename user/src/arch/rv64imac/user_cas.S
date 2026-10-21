/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .section .text.kickos_user_cas, "ax", @progbits

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired) */
    .global kickos_user_cas
    .type kickos_user_cas, @function
    .balign 4
kickos_user_cas:
    sext.w  a1, a1                  /* lr.w sign-extends; so must the operand it meets */
1:  lr.w.aqrl a3, (a0)
    bne     a3, a1, 2f
    sc.w.rl a4, a2, (a0)
    bnez    a4, 1b
2:  mv      a0, a3
    ret
    .size kickos_user_cas, . - kickos_user_cas
