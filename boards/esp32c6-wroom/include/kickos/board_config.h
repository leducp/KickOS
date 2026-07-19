/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the ESP32-C6-WROOM-1 (ESP-RISC-V, RV32IMAC). See the
 * mk64f board_config.h for the mechanism. Pure integer macros only (also included
 * from startup.S).
 *
 * KICKOS_MAX_IRQ bounds the valid interrupt-line space. The C6 CPU takes up to 31
 * interrupts from the interrupt matrix (single mtvec demux -- no per-line startup
 * vector table). 32 covers them with margin (the exact matrix wiring is HW-pass
 * work).
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 32 /* C6 CPU interrupt lines (single mtvec demux) */
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

/* Userspace malloc arena (newlib_stubs.cc s_heap). Under full-C++ MPU enforcement it
 * routes into the granted .appdata window, so it must fit alongside libstdc++/unwind
 * writable globals + the DWARF FDE registry in the pow2 app-data region. 16K heap fits
 * a 32K app window on this 512K-RAM part. */
#ifndef KICKOS_HEAP_SIZE
#define KICKOS_HEAP_SIZE (16 * 1024)
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
