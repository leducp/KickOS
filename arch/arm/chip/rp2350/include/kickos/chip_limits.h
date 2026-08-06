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

#endif /* KICKOS_CHIP_LIMITS_H */
