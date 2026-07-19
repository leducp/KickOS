/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Genuine STM32F103C8 "Blue Pill" (medium density: 20 KiB SRAM / 64 KiB flash).
 * Chip backend = stm32f103, SHARED with the low-density clone (board "bluepill",
 * which uses the chip defaults). Same HW as the clone (PC13 LED, USART1) -- only
 * the silicon size differs, so this board just relaxes the provisioning and pairs
 * with boards/bluepill-c8/stm32f103.ld (20 KiB). HW-UNTESTED.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 43 /* STM32F103 medium-density NVIC lines */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 2
#endif
/* M3 cap table: floor 6 (holds the 4 live sem caps of the irqdrv selftest section). */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 6
#endif
/* M3 PI-mutex pool: tiny-board floor (RAM is 20 KiB). */
#ifndef KICKOS_MAX_MUTEXES
#define KICKOS_MAX_MUTEXES 4
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 2048
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 512
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 2048
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
