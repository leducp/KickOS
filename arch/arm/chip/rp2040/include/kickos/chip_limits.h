/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* RP2040: 26 lines, table rounded to 32 */
#define KICKOS_MAX_IRQ 32

/*
 * IRQ26..31 are tied to zero and never fire, yet the NVIC still enters their handler when
 * software sets ISPR (datasheet 2.3.2). Only a line with no source can have a pending
 * RETIRED: a wired line's source re-asserts and undoes the ICPR write. IRQ15 is
 * SIO_IRQ_PROC0, driven by the core-local FIFO level and not gated by an enable bit, so
 * it re-asserts even though nothing here configures the FIFO.
 */
#define KICKOS_IRQ_SOFT_ONLY_BASE 26

#endif /* KICKOS_CHIP_LIMITS_H */
