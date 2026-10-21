/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 */

    .section .text.kickos_user_cas, "ax", @progbits

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired) */
    .globl  kickos_user_cas
    .type   kickos_user_cas, @function
    .balign 16
kickos_user_cas:
    movl    %esi, %eax
    lock cmpxchgl %edx, (%rdi)
    ret
    .size kickos_user_cas, . - kickos_user_cas
