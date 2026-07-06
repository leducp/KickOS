/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the Nucleo-F302R8 (STM32F302). See stm32f103's
 * board_config.h for the mechanism. Tiny SRAM (16 KiB). Pure integer macros
 * only: also included from startup.S.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 82 /* STM32F302x8: vector table tops at position 81 */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 2
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
