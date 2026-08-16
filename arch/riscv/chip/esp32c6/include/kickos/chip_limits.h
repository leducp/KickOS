/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* C6 CPU interrupt lines (single mtvec demux) */
#define KICKOS_MAX_IRQ 32

/*
 * A logical kernel IRQ line (irq.h, kernel_line) is chip-local, not a CPU interrupt ID.
 * Only UART0_TX_LINE (16) is mapped to an interrupt-matrix source, so any other logical
 * line has none behind it and holds a pending bit software sets. Without this the arms
 * would fall on 15 and 16, and 16 is that line.
 */
#define KICKOS_IRQ_SOFT_ONLY_BASE 26

#endif /* KICKOS_CHIP_LIMITS_H */
