/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* RP2350: 52 NVIC inputs (IRQ0..51; 46..51 spare) */
#define KICKOS_MAX_IRQ 52

/*
 * IRQ46..51 are spare (datasheet RP-008373-DS-2, 3.2 Table 95): no source is wired to
 * them, so a pending bit set by software STAYS set instead of being re-asserted away.
 * That is the property the discard arm needs and the only reason this is declared.
 */
#define KICKOS_IRQ_SOFT_ONLY_BASE 46

/*
 * SIO_IRQ_BELL, the cross-core doorbell line (datasheet RP-008373-DS-2, 3.1.6). Core-local:
 * the same number on each core, each core seeing only its own. Here rather than in irq.h
 * because node_vectors.S reads it.
 */
#define KICKOS_RP2350_SIO_IRQ_BELL 26

/* Clear of USBCTRL_IRQ (14, irq.h), which the default block 6..14 would cover. */
#define KICKOS_SELFTEST_IRQ_BASE 15

#endif /* KICKOS_CHIP_LIMITS_H */
