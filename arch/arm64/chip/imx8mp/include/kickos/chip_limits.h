/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* GICD_TYPER.ITLinesNumber reads 5 on this part's GIC-500, so (5+1)*32 INTIDs, and the two
 * sources agree: IMX8MPRM rev 3 section 7.1.2 gives 160 shared peripheral interrupts, and the
 * 32 banked SGI+PPI IDs sit BELOW them rather than inside, for 192. Costs 16 bytes of kernel
 * .bss per ID. */
#define KICKOS_MAX_IRQ 192

/* The default 6 hangs the selftest's IRQ arms: GICD_ISPENDR cannot set the pending state of
 * an INTID below 32, so arch_irq_inject there is a silent no-op. The arms take this base plus
 * offsets 0 TO 10 INCLUSIVE, so the eleven IDs are 181 through 191 and they end exactly at the
 * distributor's last. Ten of them are the die's own reserved tail, section 7.1.2's table
 * assigning shared sources up to its IRQ 149, which is INTID 181, and calling IRQ 150 to 159
 * reserved; so only the first can name a source at all, and the machine wires a device to
 * none of the eleven. */
#define KICKOS_SELFTEST_IRQ_BASE 181

#endif /* KICKOS_CHIP_LIMITS_H */
