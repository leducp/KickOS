/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ESP32-C6 entry from the ROM loader, in M mode. mtvec is installed later by
 * kickos_rv32_init, so no trap may occur before then.
 */

    .section .text.init, "ax"
    .global _start
_start:
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, _estack             /* boot/handler stack: top of HP SRAM */
    li      tp, 0

    call    Reset_Handler           /* C runtime + kmain (noreturn) */
1:  j       1b
