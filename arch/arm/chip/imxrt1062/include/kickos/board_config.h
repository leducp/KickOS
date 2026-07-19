/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the Teensy 4.1 (NXP i.MX RT1062, Cortex-M7). See
 * mk64f's board_config.h for the mechanism. Large on-chip RAM (OCRAM2 512 KiB
 * used for the first bring-up). Pure integer macros only: also included from
 * startup.S.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 160 /* i.MX RT1060 CM7: IRQ0..159 (RM ch.4) */
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

#endif /* KICKOS_BOARD_CONFIG_H */
