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
/* M3 cap table floor: FIRST_DYNAMIC(4) reserved + main's 2 permanent caps
   (g_done/g_lock) + 3 concurrent own-caps (cap_index0 holds sem+endpoint+mutex) = 9.
   Below this the reduced selftest suite exhausts main's dynamic slots. */
#ifndef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES 9
#endif
/* Every default stack here is bounded by the arena, which on 16 KiB is what is left
   after the image's static footprint: KICKOS_MAX_THREADS x this size must still fit
   past the two boot stacks (boot_arena.ld.h asserts it), so these are provisioning
   facts, not comfort margins. Measured on silicon (paint-and-scan watermarks, whole
   selftest suite): deepest pool worker 592 B, root 1048 B, idle 76 B. */
#ifndef KICKOS_USER_STACK_SIZE
#define KICKOS_USER_STACK_SIZE 1024
#endif
/* (f302 is not an enforcement target: its 16 KiB SRAM cannot hold the app-data
   block + arena. Default stacks are demand-allocated from the arena, not a pool.) */
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 512
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 1536
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
