/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` RV32 startup, entered in machine mode at the RAM origin. No trap may occur
 * before kickos_rv32_init installs mtvec.
 */

    .section .text.init, "ax"
    .global _start
_start:
    /* Relaxation OFF: a relaxed la would be gp-relative before gp is set. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, _estack
    li      tp, 0

    call    Reset_Handler
1:  j       1b
