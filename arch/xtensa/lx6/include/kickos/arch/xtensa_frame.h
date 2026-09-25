/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The Xtensa LX6 level-1 interrupt-frame layout, indexed by both the save/restore asm
 * (arch/xtensa/chip/esp32/startup.S) and the C++ fabricator arch_context_init
 * (arch/xtensa/lx6/arch_xtensa.cc), so the offsets live here once.
 *
 * Asm-safe: plain object and function-like #defines only, with no C types and no literal
 * suffixes gas cannot parse. No C struct mirrors the frame, so nothing static_asserts the F_*
 * offsets.
 */
#ifndef KICKOS_ARCH_XTENSA_FRAME_H
#define KICKOS_ARCH_XTENSA_FRAME_H

#define F_PC   0x00
#define F_PS   0x04
#define F_SAR  0x08
#define F_LBEG 0x0C
#define F_LEND 0x10
#define F_LCNT 0x14

#define F_A0      0x20
#define F_AREG(n) (F_A0 + (n) * 4)
#define F_A1      F_AREG(1)
#define F_A2      F_AREG(2)
#define F_A3      F_AREG(3)
#define F_A4      F_AREG(4)
#define F_A5      F_AREG(5)
#define F_A6      F_AREG(6)
#define F_A7      F_AREG(7)
#define F_A8      F_AREG(8)
#define F_A9      F_AREG(9)
#define F_A10     F_AREG(10)
#define F_A11     F_AREG(11)
#define F_A12     F_AREG(12)
#define F_A13     F_AREG(13)
#define F_A14     F_AREG(14)
#define F_A15     F_AREG(15)

/* Single-precision FPU (CP0): f0..f15 contiguous, 4 bytes each, so through 0x9C, then FCR and
   FSR. Banked only on the preemptive path. */
#define F_F0   0x60
#define F_FCR  0xA0
#define F_FSR  0xA4

/* Frame size, 16-byte aligned. */
#define F_SIZE 0x100

/* arch_context.tls_base (context.h), which trace_tid precedes where telemetry is compiled. */
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#define KICKOS_LX6_CTX_TLS_BASE 20
#else
#define KICKOS_LX6_CTX_TLS_BASE 16
#endif

#ifdef __ASSEMBLER__
#if defined(KICKOS_TLS) && KICKOS_TLS
/* Seat THREADPTR for the thread whose context pointer is in \ctx. EVERY SWITCH-IN INVOKES THIS,
 * the preemptive one at the tail of _kickos_int_level1 included: a missed site leaves the
 * incoming thread on the outgoing thread's thread_local storage and libc state. */
    .macro  SEAT_THREADPTR ctx, tmp
    l32i    \tmp, \ctx, KICKOS_LX6_CTX_TLS_BASE
    wur.threadptr \tmp
    .endm
#else
    .macro  SEAT_THREADPTR ctx, tmp
    .endm
#endif
#endif // __ASSEMBLER__

#endif
