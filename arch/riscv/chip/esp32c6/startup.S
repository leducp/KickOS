/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ESP32-C6-WROOM-1 (ESP-RISC-V, RV32IMAC) startup. The ROM first-stage loader
 * copies the image segments into HP SRAM and enters at _start in MACHINE mode.
 * _start sets the global pointer + boot stack, then hands off to the C runtime
 * (Reset_Handler: .data/.bss/init_array + arch_init + kmain). mtvec is installed
 * later by kickos_rv32_init (from arch_init).
 *
 * Validated on ESP32-C6 silicon.
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
