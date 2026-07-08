/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` (RISC-V RV32IMAC) startup. QEMU with `-bios none -kernel <elf>`
 * enters at the image's start (0x8000_0000, RAM origin) in MACHINE mode. _start
 * sets the global pointer + the boot stack, then hands off to the C runtime
 * (Reset_Handler: .data/.bss/init_array + arch_init + kmain). mtvec is installed
 * later by kickos_rv32_init (from arch_init), so no trap must occur before then.
 */

    .section .text.init, "ax"
    .global _start
_start:
    /* gp := __global_pointer$ with relaxation OFF (a relaxed la would itself be
       gp-relative, but gp is not set yet). Standard RISC-V boot idiom. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, _estack             /* boot/handler stack: top of RAM */
    li      tp, 0                   /* no TLS: thread pointer parked at 0 */

    call    Reset_Handler           /* C runtime + kmain (noreturn) */
1:  j       1b                      /* Reset_Handler never returns */
