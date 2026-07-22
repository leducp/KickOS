/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Single source of truth for the Xtensa LX6 level-1 interrupt-frame layout: the
 * 256-byte frame built on the interruptee's stack by the level-1 interrupt entry
 * and torn down by _kickos_lx6_irq_restore (arch/xtensa/chip/esp32/startup.S), and
 * fabricated for a fresh thread's first resume by arch_context_init (arch/xtensa/
 * lx6/arch_xtensa.cc). Both the save/restore asm and the C++ fabricator index this
 * frame, so the offsets live here once instead of being hand-synced across the two
 * files. Asm-safe: plain object/function-like #defines only -- no C types and no
 * literal suffixes gas cannot parse -- includable from both a .cc and a cpp-
 * processed .S.
 *
 * Pure-asm frame: no C struct mirrors it, so there is nothing to static_assert the
 * offsets against (unlike the arch_context CTX_* offsets, which context.h defines
 * and arch_xtensa.cc anchors to struct arch_context via offsetof static_asserts).
 */
#ifndef KICKOS_ARCH_XTENSA_FRAME_H
#define KICKOS_ARCH_XTENSA_FRAME_H

/* Special registers saved at the head of the frame. */
#define F_PC   0x00
#define F_PS   0x04
#define F_SAR  0x08
#define F_LBEG 0x0C
#define F_LEND 0x10
#define F_LCNT 0x14

/* General (address) registers a0..a15: contiguous, 4 bytes each. F_AREG(n) is the
   stride relation (single source); the named F_A1..F_A15 derive from it. */
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

/* Single-precision FPU (CP0) save area: f0..f15 contiguous, 4 bytes each (-> 0x9C),
   then FCR, FSR. Banked only on the preemptive path. */
#define F_F0   0x60
#define F_FCR  0xA0
#define F_FSR  0xA4

/* Total frame size (16-byte aligned). */
#define F_SIZE 0x100

#endif
