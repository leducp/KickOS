/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Genuine STM32F103C8 "Blue Pill" (medium density: 20 KiB SRAM / 64 KiB flash).
 * Chip backend = stm32f103, SHARED with the low-density clone (board "bluepill",
 * which uses the chip defaults). Same HW as the clone (PC13 LED, USART1) -- only
 * the silicon size differs, so this board just relaxes the provisioning and pairs
 * with boards/bluepill-c8/stm32f103.ld (20 KiB).
 * Validation status of this port: see docs/reference/boards.md.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 43 /* STM32F103 medium-density NVIC lines */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 2
#endif
/* Tier-1 IRQ bindings: this board links no drivers (service list is none, the console is
   the in-kernel polled chip backend) and the reduced suite never holds more than one
   claimed line at a time. Each slot costs 24 B of Kernel .bss, which comes straight off a
   20 KiB part's boot arena -- at the fleet default of 8 this image does not link. */
#ifndef KICKOS_MAX_IRQ_HANDLES
#define KICKOS_MAX_IRQ_HANDLES 4
#endif
/* M3 cap table floor: FIRST_DYNAMIC(2) reserved + main's 2 permanent caps
   (g_done/g_lock) + 3 concurrent own-caps (cap_index0 holds sem+endpoint+mutex) = 7.
   Below this the reduced selftest suite exhausts main's dynamic slots. */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 7
#endif
/* M3 PI-mutex pool: tiny-board floor (RAM is 20 KiB). */
#ifndef KICKOS_MAX_MUTEXES
#define KICKOS_MAX_MUTEXES 4
#endif
/* This header REPLACES the chip's (the lookup is either/or), so a knob the chip header
   right-sized and this one omits silently reverts to the fleet default. That is how this
   board carried KICKOS_MAX_SEMAPHORES 16 instead of 4: 12 spare Semaphore slots of kernel
   .bss on a 20 KiB part. */
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 4
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
