/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the RP2350 (Cortex-M33). See stm32f103's
 * board_config.h for the mechanism. Large SRAM (520 KiB; 512 KiB used). Pure
 * integer macros only: also included from startup.S.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 52 /* RP2350: 52 NVIC inputs (IRQ0..51; 46..51 spare) */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 8192
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 2048
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 8192
#endif
#ifndef KICKOS_HEAP_SIZE
#define KICKOS_HEAP_SIZE (16 * 1024) /* libc heap arena (malloc/new); routes into the app window once MPU enforcement lands */
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
