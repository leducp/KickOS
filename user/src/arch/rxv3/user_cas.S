/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * SWINT resumes a thread switched out in [_kickos_user_cas, _kickos_user_cas_store] at
 * _kickos_user_cas: nothing in that window may overwrite its own input.
 */

    .section .text.kickos_user_cas, "ax"

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired): R1, R2, R3 */
    .global _kickos_user_cas
    .type _kickos_user_cas, @function
_kickos_user_cas:
    mov.l   [r1], r5
    cmp     r2, r5
    bne     .Lucas_done
    .global _kickos_user_cas_store
_kickos_user_cas_store:
    mov.l   r3, [r1]
.Lucas_done:
    mov.l   r5, r1
    rts
    .size _kickos_user_cas, . - _kickos_user_cas
