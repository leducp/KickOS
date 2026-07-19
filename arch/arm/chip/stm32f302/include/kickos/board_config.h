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
/* 16 KiB SRAM, 2 threads: right-size the sem/irq-handle pools down from the
   system.h 16/8 defaults (reclaims ~300 B BSS; the linker ASSERT would otherwise
   just have to fit them). Mirrors the nRF51 (same 16 KiB part). */
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 4
#endif
#ifndef KICKOS_MAX_IRQ_HANDLES
#define KICKOS_MAX_IRQ_HANDLES 4
#endif
/* M3 cap table: floor 6 (holds the 4 live sem caps of the irqdrv section). */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 6
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 2048
#endif
/* (f302 is not an enforcement target: its 16 KiB SRAM cannot hold the app-data
   block + arena. Default stacks are demand-allocated from the arena, not a pool.) */
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 512
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 2048
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
