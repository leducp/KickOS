// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 PLL_SYS register map (RP2040 datasheet, RP-008371-DS, 2.18). The PLL
// reference is the XOSC directly (not clk_ref): 12 / REFDIV x FBDIV = VCO, then /
// POSTDIV1 / POSTDIV2. 12 / 1 * 125 = 1500 MHz VCO (in 400..1600), /6 /2 = 125 MHz.
// Load REFDIV + FBDIV before powering the VCO; enable the post-dividers after lock.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::pll
{
    constexpr uintptr_t CS = mmap::PLL_SYS_BASE + 0x0u;
    constexpr uintptr_t PWR = mmap::PLL_SYS_BASE + 0x4u;
    constexpr uintptr_t FBDIV_INT = mmap::PLL_SYS_BASE + 0x8u;
    constexpr uintptr_t PRIM = mmap::PLL_SYS_BASE + 0xcu;

    constexpr uint32_t CS_LOCK = 1u << 31;
    constexpr uint32_t CS_REFDIV_1 = 1u;   // CS.REFDIV[5:0]
    constexpr uint32_t FBDIV_125 = 125u;   // FBDIV_INT[11:0]

    constexpr uint32_t PWR_PD = 1u << 0;
    constexpr uint32_t PWR_POSTDIVPD = 1u << 3;
    constexpr uint32_t PWR_VCOPD = 1u << 5;

    constexpr uint32_t PRIM_POSTDIV = (6u << 16) | (2u << 12); // POSTDIV1=6, POSTDIV2=2
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H
