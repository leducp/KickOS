/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Per-board provisioning for the QEMU `virt` RISC-V board (RV32IMAC, M-mode).
 * See the mk64f board_config.h for the mechanism. Pure integer macros only (also
 * included from startup.S).
 *
 * KICKOS_MAX_IRQ bounds the valid interrupt-line space (irq_attach / mask). Unlike
 * the ARM ports there is no per-line startup vector table: RISC-V uses a single
 * `mtvec` trap that demuxes on `mcause` (switch.S). 32 covers the standard mcause
 * interrupt-code range with headroom for the bench's spare line.
 */
#ifndef KICKOS_BOARD_CONFIG_H
#define KICKOS_BOARD_CONFIG_H

#ifndef KICKOS_MAX_IRQ
#define KICKOS_MAX_IRQ 32 /* RISC-V mcause interrupt-code space (single mtvec demux) */
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
 * writable globals + the DWARF FDE registry in the pow2 app-data region. Without this
 * the full-C++ cxxtest link pulls the newlib default 64K heap, which no sane pow2 app
 * window can hold; 16K heap fits a 32K app window (mirrors esp32c6-wroom). */
#ifndef KICKOS_HEAP_SIZE
#define KICKOS_HEAP_SIZE (16 * 1024)
#endif

#endif /* KICKOS_BOARD_CONFIG_H */
