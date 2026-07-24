// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 WATCHDOG register map (RP2040 datasheet, RP-008371-DS, 4.7). Only the
// TICK generator is used here: it divides clk_ref down to feed the 1 MHz system
// TIMER. clk_ref is 12 MHz (XOSC) normally, ~6.5 MHz (ROSC) in the fallback, so
// CYCLES is chosen to land near 1 MHz either way.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_WATCHDOG_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_WATCHDOG_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::watchdog
{
    constexpr uintptr_t TICK = mmap::WATCHDOG_BASE + 0x2cu;

    // tick = clk_ref / CYCLES; ENABLE = bit 9.
    constexpr uint32_t TICK_CFG = (1u << 9) | 12u;     // 12 MHz / 12 = 1 MHz
    constexpr uint32_t TICK_CFG_ROSC = (1u << 9) | 7u; // ~6.5 MHz / 7 ~= 1 MHz
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_WATCHDOG_H
