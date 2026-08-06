// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 XOSC register map (RP2350 datasheet RP-008373-DS-2, 8.2): 12 MHz crystal
// oscillator. Offsets are XOSC_BASE-relative (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_XOSC_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_XOSC_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::xosc
{
    constexpr uintptr_t CTRL = mmap::XOSC_BASE + 0x0u;
    constexpr uintptr_t STATUS = mmap::XOSC_BASE + 0x4u;
    constexpr uintptr_t STARTUP = mmap::XOSC_BASE + 0xcu;

    constexpr uint32_t FREQ_1_15MHZ = 0xaa0u;      // CTRL.FREQ_RANGE (1..15 MHz, 12 MHz)
    constexpr uint32_t ENABLE = 0xfabu << 12;      // CTRL.ENABLE magic
    constexpr uint32_t STATUS_STABLE = 1u << 31;   // STATUS.STABLE
    // STARTUP.DELAY counts in units of 256 crystal periods; ceil(12e6*1ms/256)=47.
    constexpr uint32_t STARTUP_DELAY = 47u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_XOSC_H
