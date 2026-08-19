// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 TICKS register map (RP2350 datasheet RP-008373-DS-2, 8.5): the new common
// tick generators. The 1 us system TIMER0 tick comes from the TIMER0 generator
// here, not the watchdog (the RP2040 model). Offsets are TICKS_BASE-relative
// (see mmap.h). The generator must be stopped before CYCLES is changed (8.5.1).

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

    constexpr uint32_t CTRL_ENABLE = 1u << 0;
    // tick = clk_ref / CYCLES, and clk_ref is whatever CLK_REF_DIV leaves, so CYCLES is
    // derived from the live divisor rather than written as a constant.
    constexpr uint32_t TICK_HZ = 1000000u; // arch_clock_now reads the count as microseconds
    constexpr uint32_t CYCLES_MAX = 0x1ffu; // CYCLES is 9 bits (DS 8.5.1)
}

#endif
