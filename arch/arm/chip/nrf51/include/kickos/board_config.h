/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the BBC micro:bit (nRF51822, Cortex-M0). See
 * stm32f103's board_config.h for the mechanism. Tiny SRAM (16 KiB). Pure
 * integer macros only: also included from startup.S.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 32 /* nRF51 (matches its startup vector table) */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 2
#endif
/* 16 KiB SRAM, 2 threads: right-size the sem/irq-handle pools down from the
   system.h 16/8 defaults (reclaims ~300 B BSS; the linker ASSERT would otherwise
   just have to fit them). */
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 4
#endif
#ifndef KICKOS_MAX_IRQ_HANDLES
#define KICKOS_MAX_IRQ_HANDLES 4
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
