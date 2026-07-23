/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the Blue Pill (STM32F103). CMake adds this dir to
 * the include path; pulled in by kickos/config/{board,system}.h and by the chip
 * startup.S. MAX_IRQ is a chip fact (NVIC line count) and drives the startup
 * vector table's .rept -- one fact, one place. The pool/stack sizes are
 * board/SRAM facts. Every knob is #ifndef-guarded so a CMake -D still wins.
 * Pure integer macros only: this header is also included from .S.
 *
 * Sized to the LOW-DENSITY floor (10 KiB SRAM); see stm32f103.ld.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 43 /* STM32F103 medium-density: 43 maskable NVIC lines */
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 2
#endif
/* 10-20 KiB SRAM, 2 threads: right-size the sem/irq-handle pools down from the
   system.h 16/8 defaults (reclaims ~300 B BSS). */
#ifndef KICKOS_MAX_SEMAPHORES
#define KICKOS_MAX_SEMAPHORES 4
#endif
/* M3 cap table floor: FIRST_DYNAMIC(4) reserved + main's 2 permanent caps
   (g_done/g_lock) + 3 concurrent own-caps (cap_index0 holds sem+endpoint+mutex) = 9.
   Below this the reduced selftest suite exhausts main's dynamic slots. */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 9
#endif
#ifndef KICKOS_MAX_IRQ_HANDLES
#define KICKOS_MAX_IRQ_HANDLES 4
#endif
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 1024
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 512
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 1024
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
