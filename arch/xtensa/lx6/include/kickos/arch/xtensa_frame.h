/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The Xtensa LX6 level-1 interrupt-frame layout, indexed by both the save/restore asm
 * (arch/xtensa/chip/esp32/startup.S) and the C++ fabricator arch_context_init
 * (arch/xtensa/lx6/arch_xtensa.cc), so the offsets live here once.
 *
 * Asm-safe: plain object and function-like #defines only, with no C types and no literal
 * suffixes gas cannot parse, so this is includable from both a .cc and a cpp-processed .S.
 * No C struct mirrors the frame, so there is nothing to static_assert these offsets against.
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

#ifdef __ASSEMBLER__
#if defined(KICKOS_TLS) && KICKOS_TLS
/* Seat THREADPTR for the thread whose context pointer is in \ctx, from that thread's own
 * saved sp masked down to its stack block. Xtensa TLS is variant 1, so THREADPTR is the block
 * base and the compiler adds the offset the linker computed.
 *
 * THE SAVED sp IS ALWAYS THAT THREAD'S OWN: lx6 selects no ARCH_HAS_KERNEL_STACKS, so there is
 * no second stack a frame could be sitting on when it is saved. If that changes this becomes
 * wrong in the same way masking F_SP is wrong on rv32imac.
 *
 * The -1 is the edge of the range: a stack top is EXCLUSIVE, so an empty stack has sp exactly
 * at base + stride and a plain mask returns the NEXT block.
 *
 * movi with the negated stride rather than srli/slli: srli takes an immediate of 0..15 and a
 * board with a larger stack would silently be out of range.
 *
 * THREE SITES INVOKE THIS: xtensa_switch and arch_start in switch.S, and the PREEMPTIVE switch
 * at the tail of _kickos_int_level1, which never enters xtensa_switch. Seating it in switch.S
 * alone leaves the incoming thread on the OUTGOING thread's pointer until its next
 * cooperative switch.
 *
 * IDLE GETS AN ADDRESS INSIDE A NEIGHBOUR'S BLOCK, its stack being below one stride and taking
 * no carve. Its body is arch_idle_wait and it reaches no thread_local; the no_privileged_tls
 * gate is what keeps that true. */
    .macro  SEAT_THREADPTR ctx, tmp0, tmp1
    l32i    \tmp0, \ctx, CTX_SP
    addi    \tmp0, \tmp0, -1
    movi    \tmp1, -KICKOS_TLS_STRIDE
    and     \tmp0, \tmp0, \tmp1
    wur.threadptr \tmp0
    .endm
#else
    .macro  SEAT_THREADPTR ctx, tmp0, tmp1
    .endm
#endif
#endif // __ASSEMBLER__

#endif
