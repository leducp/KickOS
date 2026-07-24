// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 SIO (single-cycle IO) GPIO register map (RP2040 datasheet, RP-008371-DS,
// 2.3.1). SIO lives on the core-local IOPORT bus, NOT the APB, so it does NOT use
// the +0x2000/+0x3000 atomic alias window (regs/atomic.h). Instead each GPIO_OUT /
// GPIO_OE register has its own dedicated SET / CLR / XOR sibling register. A pin
// must first be muxed to FUNCSEL_SIO (regs/io_bank0.h) before SIO drives it.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_SIO_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_SIO_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::sio
{
    constexpr uintptr_t GPIO_OUT = mmap::SIO_BASE + 0x10u;
    constexpr uintptr_t GPIO_OUT_SET = mmap::SIO_BASE + 0x14u;
    constexpr uintptr_t GPIO_OUT_CLR = mmap::SIO_BASE + 0x18u;
    constexpr uintptr_t GPIO_OUT_XOR = mmap::SIO_BASE + 0x1cu;
    constexpr uintptr_t GPIO_OE = mmap::SIO_BASE + 0x20u;
    constexpr uintptr_t GPIO_OE_SET = mmap::SIO_BASE + 0x24u;
    constexpr uintptr_t GPIO_OE_CLR = mmap::SIO_BASE + 0x28u;
    constexpr uintptr_t GPIO_OE_XOR = mmap::SIO_BASE + 0x2cu;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_SIO_H
