// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 TICKS register map (RP2350 datasheet RP-008373-DS-2, 8.5). The TIMER0 generator feeds
// the 1 us system TIMER0 tick.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TICKS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TICKS_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::ticks
{
    // TIMER0 generator triplet: CTRL 0x18, CYCLES 0x1c, COUNT 0x20.
    constexpr uintptr_t TIMER0_CTRL = mmap::TICKS_BASE + 0x18u;
    constexpr uintptr_t TIMER0_CYCLES = mmap::TICKS_BASE + 0x1cu;
    constexpr uintptr_t TIMER0_COUNT = mmap::TICKS_BASE + 0x20u;

    // tick = clk_ref / CYCLES.
    constexpr uint32_t CTRL_ENABLE = 1u << 0;
}

#endif
