/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the XMC4800 Relax Kit (XMC4800). See stm32f103's
 * board_config.h for the mechanism. Mid SRAM (128 KiB DSRAM1). Pure integer
 * macros only: also included from startup.S.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 112 /* XMC4800: IRQ0..IRQ111 */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 8
#endif
/* Root's cap table must cover the board service list's RETAINED caps, not just
   FIRST_DYNAMIC: xmcssc keeps its SPI request endpoint for the life of the image.
   Budget: 2 reserved + 2 permanent selftest caps + 1 retained SPI ep + 6 concurrent in
   t_mutex_deadlock = 11, so the fleet default of 10 already fails. Exhaustion surfaces
   only as a mislabelled "SKIP pool too small", because -KOS_ENOMEM cannot distinguish
   a full cap table from an empty pool. */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 12
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

#endif /* KICKOS_BOARD_CONFIG_H */
