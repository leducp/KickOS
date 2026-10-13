// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 WATCHDOG register map (RP2040 datasheet, RP-008371-DS, 4.7). Only the TICK generator
// is used here: it divides clk_ref down to feed the 1 MHz system TIMER.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_WATCHDOG_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_WATCHDOG_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::watchdog
{
    constexpr uintptr_t CTRL = mmap::WATCHDOG_BASE + 0x00u;
    constexpr uintptr_t SCRATCH4 = mmap::WATCHDOG_BASE + 0x1cu;
    constexpr uintptr_t TICK = mmap::WATCHDOG_BASE + 0x2cu;

    constexpr uint32_t CTRL_TRIGGER = 1u << 31;
    constexpr uint32_t CTRL_ENABLE = 1u << 30;

    // tick = clk_ref / CYCLES, CYCLES in bits 8:0.
    constexpr uint32_t TICK_ENABLE = 1u << 9;
}

// PSM (2.13): the blocks a watchdog reset takes down. Every one but the two oscillators, so
// the restart runs on the clock the reset left.
namespace kickos::rp2040::reg::psm
{
    constexpr uintptr_t WDSEL = mmap::PSM_BASE + 0x08u;
    constexpr uint32_t WDSEL_ALL_BUT_OSCILLATORS = 0x0001fffcu;
}

#endif
