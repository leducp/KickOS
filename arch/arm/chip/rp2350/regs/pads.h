// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 PADS_BANK0 register map (RP2350 datasheet RP-008373-DS-2, 9.11.3):
// per-pin pad control. Offsets are PADS_BANK0_BASE-relative (see mmap.h). RP2350
// pads reset ISOLATED (ISO set) -- clear ISO or the pad stays disconnected (an
// RP2040 delta).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_PADS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_PADS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2350::reg::pads
{
    // GPIOn = PADS_BANK0_BASE + 0x04 + n*0x04.
    constexpr uintptr_t GPIO_BASE = mmap::PADS_BANK0_BASE + 0x04u;
    constexpr uintptr_t GPIO_STRIDE = 0x04u;
    constexpr uintptr_t GPIO4 = mmap::PADS_BANK0_BASE + 0x14u;
    constexpr uintptr_t GPIO5 = mmap::PADS_BANK0_BASE + 0x18u;

    constexpr uintptr_t gpio(uint32_t n) { return GPIO_BASE + n * GPIO_STRIDE; }

    constexpr uint32_t ISO = 1u << 8; // pad isolation (resets SET -- clear to use)
    constexpr uint32_t OD = 1u << 7;  // output disable
    constexpr uint32_t IE = 1u << 6;  // input enable
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_PADS_H
