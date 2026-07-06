/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * STM32F411E-DISCO board facts. Chip backend = stm32f411, SHARED with the WeAct
 * "Black Pill" (boards/blackpill) -- this is the board/chip split: same SoC
 * driver, per-board HW here. CMake selects boards/<board>/include over the chip
 * default. Pure integer macros (also read by the chip clock/LED code + startup.S);
 * all #ifndef-guarded so a CMake -D still overrides.
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
#define KICKOS_HSE_HZ 8000000 /* on-board 8 MHz HSE crystal */
#endif
/* Diagnostic LED: LD4 (green) = PD12, active-high. */
#ifndef KICKOS_LED_GPIO
#define KICKOS_LED_GPIO 0x40020C00 /* GPIOD */
#endif
#ifndef KICKOS_LED_RCC_AHB1_BIT
#define KICKOS_LED_RCC_AHB1_BIT 3 /* GPIODEN */
#endif
#ifndef KICKOS_LED_PIN
#define KICKOS_LED_PIN 12
#endif
#ifndef KICKOS_LED_ACTIVE_LOW
#define KICKOS_LED_ACTIVE_LOW 0
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
