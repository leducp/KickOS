/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * The board's crystal, the one wiring fact its board file has no field for. Unconditional
 * #define, as chip_limits.h: nothing configures it and no option depends on it.
 */
#ifndef KICKOS_BOARD_WIRING_H
#define KICKOS_BOARD_WIRING_H

/* WeAct STM32F411: 25 MHz crystal */
#define KICKOS_HSE_HZ 25000000

#endif /* KICKOS_BOARD_WIRING_H */
