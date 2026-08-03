/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for a Renesas RX72M board (RX72M Group, RXv3). See the
 * mk64f board_config.h for the mechanism. Pure integer macros only: also
 * included from startup.S (the vector-table .rept keys off KICKOS_MAX_IRQ).
 *
 * The RX ICUD relocatable interrupt vector table (INTB) has 256 entries; the
 * software-INT slots 0..15 carry no peripheral source (IRn is defined only for
 * n = 16..255, UM sec.15.2.1) so the full table is emitted (spike sec.5).
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

/* The INTB table size in words. Its own macro, NOT KICKOS_MAX_IRQ: the line space is
 * larger than the vector space on this chip (see below), and a table sized from the
 * line count would emit 160 words of flash past the end of a table the hardware
 * indexes with 8 bits. */
#ifndef KICKOS_RX_INTB_ENTRIES
#define KICKOS_RX_INTB_ENTRIES 256
#endif

/* 256 ICU vectors, plus 5*32 GROUP-SOURCE logical lines based at 256: a group source
 * has no vector of its own and is masked at GENxxx.ENj, so it is addressed above the
 * vector space (<kickos/arch/rx_group.h>). TEI6 = 268, ERI6 = 269.
 * Cost: Kernel::irq_table is 8 bytes per line on RX, so this is +1280 bytes of .bss
 * over the 256-line table, on a 1 MB-SRAM part. */
#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 416 /* 256 ICU vectors + 5 group registers x 32 sources */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 8192
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 2048
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 8192
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
