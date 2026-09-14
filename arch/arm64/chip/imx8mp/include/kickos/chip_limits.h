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

/* The default 6 is a SILENT no-op here: GICD_ISPENDR cannot set the pending state of an
 * INTID below 32, so arch_irq_inject on one returns having done nothing. The block is this
 * base plus offsets 0 TO 10 INCLUSIVE, so the eleven IDs are 181 through 191 and they end
 * exactly at the distributor's last. Ten of them are the die's own reserved tail, section 7.1.2's table
 * assigning shared sources up to its IRQ 149, which is INTID 181, and calling IRQ 150 to 159
 * reserved; so only the first can name a source at all, and the machine wires a device to
 * none of the eleven. */
#define KICKOS_IRQ_FREE_BASE 181

/* Rate of the counter the bench reads. PMCCNTR_EL0 counts core cycles on this part and the
 * part reports no core clock, so nothing converts a reading into time. 0 drops the
 * nanosecond columns instead. */
#define KICKOS_CHIP_CYCCNT_HZ 0

#endif /* KICKOS_CHIP_LIMITS_H */
