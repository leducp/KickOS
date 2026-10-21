/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * No MEMW: S32C1I is both acquire and release (Xtensa ISA 4.3.13.5).
 * A failed S32C1I may answer ~SCOMPARE1 rather than the word (Xtensa ISA 8.3.281), so the
 * answer on failure is a fresh load, and a load finding expected again retries.
 */

    .section .text.kickos_user_cas, "ax"

/* uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired): a2, a3, a4 */
    .global kickos_user_cas
    .type kickos_user_cas, @function
    .align 4
kickos_user_cas:
    entry   a1, 32
    wsr.scompare1 a3
1:  mov     a5, a4
    s32c1i  a5, a2, 0
    beq     a5, a3, 2f
    l32ai   a5, a2, 0
    beq     a5, a3, 1b
2:  mov     a2, a5
    retw
    .size kickos_user_cas, . - kickos_user_cas
