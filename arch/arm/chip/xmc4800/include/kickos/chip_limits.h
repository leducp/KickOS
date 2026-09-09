/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 *
 * Included from startup.S too, so pure integer macros only.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* XMC4800: IRQ0..IRQ111 */
#define KICKOS_MAX_IRQ 112

/* DWT CYCCNT is unreliable on this part (chip_xmc4800.cc), and a glitched read can only
 * inflate a delta. The bench reads MIN here where every other part reports a distribution. */
#define KICKOS_CHIP_CYCCNT_GLITCHES 1

#endif /* KICKOS_CHIP_LIMITS_H */
