/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* 256 ICU vectors, plus 5*32 GROUP-SOURCE logical lines based at 256: a group source
 * has no vector of its own and is masked at GENxxx.ENj, so it is addressed above the
 * vector space (<kickos/arch/rx_group.h>). TEI6 = 268, ERI6 = 269.
 * Cost: Kernel::irq_table is 8 bytes per line on RX, so this is +1280 bytes of .bss
 * over the 256-line table, on a 1 MB-SRAM part. */
#define KICKOS_MAX_IRQ 416

/* The INTB table size in words. Its own macro, NOT KICKOS_MAX_IRQ: the line space is
 * larger than the vector space on this chip, and a table sized from the line count
 * would emit 160 words of flash past the end of a table the hardware indexes with 8
 * bits. The software-INT slots 0..15 carry no peripheral source (IRn is defined only
 * for n = 16..255, UM sec.15.2.1) so the full table is emitted. */
#define KICKOS_RX_INTB_ENTRIES 256

#endif /* KICKOS_CHIP_LIMITS_H */
