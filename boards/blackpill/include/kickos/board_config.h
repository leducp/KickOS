/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * WeAct STM32F411CEU6 "Black Pill" board facts. Chip backend = stm32f411, SHARED
 * with the F411E-DISCO (boards/f411disco) -- the SECOND board on this chip, and
 * the concrete case the board/chip split was built for. Differs from the Disco
 * in HSE crystal (25 MHz vs 8) and LED (PC13 active-low vs PD12 active-high).
 * HW-UNTESTED: added build-only; flash to confirm (USB-DFU with BOOT0, or SWD).
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 86 /* STM32F411 NVIC lines (chip fact) */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 8
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 4096
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 2048
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 4096
#endif

/* --- Board hardware consumed by the stm32f411 backend --- */
#ifndef KICKOS_HSE_HZ
#define KICKOS_HSE_HZ 25000000 /* WeAct board: 25 MHz HSE crystal */
#endif
/* Diagnostic LED: PC13, active-low (lit when driven low). */
#ifndef KICKOS_LED_GPIO
#define KICKOS_LED_GPIO 0x40020800 /* GPIOC */
#endif
#ifndef KICKOS_LED_RCC_AHB1_BIT
#define KICKOS_LED_RCC_AHB1_BIT 2 /* GPIOCEN */
#endif
#ifndef KICKOS_LED_PIN
#define KICKOS_LED_PIN 13
#endif
#ifndef KICKOS_LED_ACTIVE_LOW
#define KICKOS_LED_ACTIVE_LOW 1
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
