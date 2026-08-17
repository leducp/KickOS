// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 SIO (single-cycle IO) GPIO register map (RP2350 datasheet RP-008373-DS-2,
// 3.1.11). SIO lives on the core-local bus at 0xD0000000, NOT the APB, so it does
// NOT use the mmap.h ATOMIC_SET/CLR aliases; each GPIO_OUT / GPIO_OE register has its
// own dedicated SET / CLR sibling. Bank-0 (GP0..GP31) registers only; the HI_* bank
// (GP32..GP47, QFN-80 parts) is not mapped. A pin must first be muxed to the SIO
// funcsel (regs/io_bank0.h) before SIO drives it.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_SIO_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_SIO_H

#include <stdint.h>

namespace kickos::rp2350::reg::sio
{
    constexpr uintptr_t BASE = 0xD0000000u;

    constexpr uintptr_t GPIO_OUT_SET = BASE + 0x018u; // set 1s in GPIO_OUT (bank 0)
    constexpr uintptr_t GPIO_OUT_CLR = BASE + 0x020u; // clear 1s in GPIO_OUT (bank 0)
    constexpr uintptr_t GPIO_OE_SET = BASE + 0x038u;  // set 1s in GPIO_OE (output enable, bank 0)
}

#endif
