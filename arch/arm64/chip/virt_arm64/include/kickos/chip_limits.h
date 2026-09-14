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

/* The default 6 is a SILENT no-op here: GICD_ISPENDR cannot set the pending state of an
 * INTID below 32, so arch_irq_inject on one returns having done nothing and every arm that
 * injects reports a clean zero. 200 is an SPI QEMU `virt` wires nothing to, its own devices
 * being INTID 32..79. */
#define KICKOS_IRQ_FREE_BASE 200

/* Rate of the counter the bench reads. Under this emulator PMCCNTR_EL0 answers the
 * emulator's own virtual clock rather than guest cycles, so no rate converts a reading into
 * guest time. 0 is what drops the nanosecond columns instead of printing them wrong. */
#define KICKOS_CHIP_CYCCNT_HZ 0

#endif /* KICKOS_CHIP_LIMITS_H */
