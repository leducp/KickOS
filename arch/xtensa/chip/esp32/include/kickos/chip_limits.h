/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define, on the model
 * of config/limits.h, because nothing configures these and no option depends on
 * them. A separate guard from board_config.h on purpose: that header is generated
 * from the configuration and would otherwise shadow this one.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* Xtensa has no NVIC: this is the count of per-CPU internal interrupt lines
 * (INTENABLE is 32-bit). Peripheral sources are mapped onto these lines by the
 * interrupt matrix (TRM 4.2). */
#define KICKOS_MAX_IRQ 32

#endif /* KICKOS_CHIP_LIMITS_H */
