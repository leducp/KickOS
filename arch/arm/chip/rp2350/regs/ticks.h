// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 TICKS register map (RP2350 datasheet RP-008373-DS-2, 8.5): the new common
// tick generators. The 1 us system TIMER0 tick comes from the TIMER0 generator
// here, not the watchdog (the RP2040 model). Offsets are TICKS_BASE-relative
// (see mmap.h). The generator must be stopped before CYCLES is changed (8.5.1).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TICKS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TICKS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2350::reg::ticks
{
    // TIMER0 generator triplet: CTRL 0x18, CYCLES 0x1c, COUNT 0x20.
    constexpr uintptr_t TIMER0_CTRL = mmap::TICKS_BASE + 0x18u;
    constexpr uintptr_t TIMER0_CYCLES = mmap::TICKS_BASE + 0x1cu;
    constexpr uintptr_t TIMER0_COUNT = mmap::TICKS_BASE + 0x20u;

    constexpr uint32_t CTRL_ENABLE = 1u << 0;
    // tick = clk_ref / CYCLES. clk_ref is 12 MHz (XOSC) normally, ~6.5 MHz (ROSC) in
    // the fallback -> pick CYCLES to land near 1 MHz either way.
    constexpr uint32_t CYCLES_12MHZ = 12u;
    constexpr uint32_t CYCLES_ROSC = 7u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_TICKS_H
