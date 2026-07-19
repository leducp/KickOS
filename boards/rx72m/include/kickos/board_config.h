/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for a Renesas RX72M board (RX72M Group, RXv3). See the
 * mk64f board_config.h for the mechanism. Pure integer macros only: also
 * included from startup.S (the vector-table .rept keys off KICKOS_MAX_IRQ).
 *
 * The RX ICUD relocatable interrupt vector table (INTB) has 256 entries; the
 * software-INT slots 0..15 carry no peripheral source (IRn is defined only for
 * n = 16..255, UM sec.15.2.1) so the full table is emitted (spike sec.5).
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 256 /* RX ICUD: 256-entry INTB vector table */
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
 * writable globals in the pow2 app-data region. Without this, newlib defaults to a 64K
 * heap, which would force the RX app window past 32K; 16K fits the 32K window like the
 * other silicon chips. */
#ifndef KICKOS_HEAP_SIZE
#define KICKOS_HEAP_SIZE (16 * 1024)
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
