/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Board wiring: facts of this board, not knobs. Unconditional #define, on the model
 * of chip_limits.h, because nothing configures these and no option depends on them.
 * The chip backend serves several boards and reads them from here, one directory per
 * board on the include path.
 *
 * Macros, not constexpr: the backend tests KICKOS_DIAG_LED_ACTIVE_LOW in an #if, and
 * a typed constant is invisible to the preprocessor.
 */
#ifndef KICKOS_BOARD_WIRING_H
#define KICKOS_BOARD_WIRING_H

/* WeAct STM32F411: 25 MHz crystal, diag LED on PC13 driven low to light */
#define KICKOS_HSE_HZ 25000000
#define KICKOS_DIAG_LED_GPIO 0x40020800
#define KICKOS_DIAG_LED_RCC_AHB1_BIT 2
#define KICKOS_DIAG_LED_PIN 13
#define KICKOS_DIAG_LED_ACTIVE_LOW 1

#endif /* KICKOS_BOARD_WIRING_H */
