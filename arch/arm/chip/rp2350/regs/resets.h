// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 RESETS register map (RP2350 datasheet RP-008373-DS-2, 7.5): peripherals
// are held in reset at power-up. Offsets are RESETS_BASE-relative (see mmap.h).
// RESET_DONE only asserts once the peripheral has a running clock.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_RESETS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_RESETS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2350::reg::resets
{
    constexpr uintptr_t RESET = mmap::RESETS_BASE + 0x0u;
    constexpr uintptr_t RESET_DONE = mmap::RESETS_BASE + 0x8u;

    // RESET / RESET_DONE bit positions (DS Table 535).
    constexpr uint32_t IO_BANK0 = 1u << 6;
    constexpr uint32_t PADS_BANK0 = 1u << 9;
    constexpr uint32_t PLL_SYS = 1u << 14;
    constexpr uint32_t TIMER0 = 1u << 23;
    constexpr uint32_t UART1 = 1u << 27;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_RESETS_H
