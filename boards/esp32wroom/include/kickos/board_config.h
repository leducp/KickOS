/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * ESP32-WROOM-32 board facts. Chip backend = esp32 (Xtensa LX6), the first
 * non-ARM board. Pure integer macros (also read
 * by the chip startup/console + the arch layer); all #ifndef-guarded so a CMake
 * -D still overrides.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
/* Xtensa has no NVIC: KICKOS_MAX_IRQ is the count of per-CPU internal interrupt
 * lines (INTENABLE is 32-bit). Peripheral sources are mapped onto these lines by
 * the interrupt matrix (TRM 4.2). */
#define KICKOS_MAX_IRQ 32
#endif
#ifndef KICKOS_MAX_THREADS
#define KICKOS_MAX_THREADS 16
#endif
#ifndef KICKOS_USER_STACK_SIZE
/* Windowed ABI: each frame can spill up to 16 ARs to the stack, so give threads
 * more headroom than the call0 draft did. */
#define KICKOS_USER_STACK_SIZE 4096
#endif
#ifndef KICKOS_IDLE_STACK_SIZE
#define KICKOS_IDLE_STACK_SIZE 2048
#endif
#ifndef KICKOS_ROOT_STACK_SIZE
#define KICKOS_ROOT_STACK_SIZE 4096
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
