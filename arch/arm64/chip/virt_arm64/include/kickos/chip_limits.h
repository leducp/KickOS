/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* GICD_TYPER reads 0x8, so (8+1)*32 IDs. 256 is the SPI count alone; the 32 banked
 * SGI+PPI IDs sit below the SPIs, not inside them. Costs 16 bytes of kernel .bss per ID. */
#define KICKOS_MAX_IRQ 288

/* The default 6 hangs the selftest's IRQ arms: GICD_ISPENDR cannot set the pending state
 * of an INTID below 32, so arch_irq_inject there is a silent no-op. 200 is an SPI QEMU
 * `virt` wires nothing to, its own devices being INTID 32..79. */
#define KICKOS_SELFTEST_IRQ_BASE 200

#endif /* KICKOS_CHIP_LIMITS_H */
