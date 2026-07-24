// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 XOSC register map (RP2040 datasheet, RP-008371-DS, 2.16): the 12 MHz
// crystal oscillator. Program FREQ_RANGE before writing ENABLE, then poll
// STATUS.STABLE.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XOSC_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XOSC_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::xosc
{
    constexpr uintptr_t CTRL = mmap::XOSC_BASE + 0x0u;
    constexpr uintptr_t STATUS = mmap::XOSC_BASE + 0x4u;
    constexpr uintptr_t STARTUP = mmap::XOSC_BASE + 0xcu;

    constexpr uint32_t FREQ_1_15MHZ = 0xaa0u;   // CTRL.FREQ_RANGE (1..15 MHz)
    constexpr uint32_t ENABLE = 0xfabu << 12;   // CTRL.ENABLE magic
    constexpr uint32_t STATUS_STABLE = 1u << 31;
    // STARTUP.DELAY counts in units of 256 crystal periods; ceil(12e6*1ms/256)=47.
    constexpr uint32_t STARTUP_DELAY = 47u;

    // ROSC reset frequency ~6.5 MHz (uncalibrated); the fallback timing source when
    // the crystal never comes up.
    constexpr uint32_t ROSC_NOMINAL_HZ = 6500000u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_XOSC_H
