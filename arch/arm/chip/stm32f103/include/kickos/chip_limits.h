/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* STM32F103 medium-density: 43 maskable NVIC lines */
#define KICKOS_MAX_IRQ 43

#endif /* KICKOS_CHIP_LIMITS_H */
