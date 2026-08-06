/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* i.MX RT1060 CM7: IRQ0..159 (RM ch.4) */
#define KICKOS_MAX_IRQ 160

#endif /* KICKOS_CHIP_LIMITS_H */
