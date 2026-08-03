// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 PLL register map (RP2040 datasheet, RP-008371-DS, 2.18). The PLL reference is
// the XOSC directly (not clk_ref): 12 / REFDIV x FBDIV = VCO, then / POSTDIV1 /
// POSTDIV2. Two instances with identical layouts.
// PLL_SYS: 12/1*125 = 1500 MHz VCO (750..1600), /6 /2 = 125 MHz.
// PLL_USB: 12/1*100 = 1200 MHz VCO, /5 /5 = 48 MHz (2.18.3).
// Load REFDIV + FBDIV before powering the VCO; enable the post-dividers after lock.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::pll
{
    constexpr uintptr_t OFF_CS = 0x0u;
    constexpr uintptr_t OFF_PWR = 0x4u;
    constexpr uintptr_t OFF_FBDIV_INT = 0x8u;
    constexpr uintptr_t OFF_PRIM = 0xcu;

    constexpr uintptr_t CS = mmap::PLL_SYS_BASE + OFF_CS;
    constexpr uintptr_t PWR = mmap::PLL_SYS_BASE + OFF_PWR;
    constexpr uintptr_t FBDIV_INT = mmap::PLL_SYS_BASE + OFF_FBDIV_INT;
    constexpr uintptr_t PRIM = mmap::PLL_SYS_BASE + OFF_PRIM;

    constexpr uint32_t CS_LOCK = 1u << 31;
    constexpr uint32_t CS_REFDIV_1 = 1u;   // CS.REFDIV[5:0]
    constexpr uint32_t FBDIV_125 = 125u;   // FBDIV_INT[11:0]

    constexpr uint32_t PWR_PD = 1u << 0;
    constexpr uint32_t PWR_POSTDIVPD = 1u << 3;
    constexpr uint32_t PWR_VCOPD = 1u << 5;

    constexpr uint32_t PRIM_POSTDIV = (6u << 16) | (2u << 12); // POSTDIV1=6, POSTDIV2=2
}

namespace kickos::rp2040::reg::pll_usb
{
    constexpr uintptr_t CS = mmap::PLL_USB_BASE + pll::OFF_CS;
    constexpr uintptr_t PWR = mmap::PLL_USB_BASE + pll::OFF_PWR;
    constexpr uintptr_t FBDIV_INT = mmap::PLL_USB_BASE + pll::OFF_FBDIV_INT;
    constexpr uintptr_t PRIM = mmap::PLL_USB_BASE + pll::OFF_PRIM;

    constexpr uint32_t FBDIV_100 = 100u;
    constexpr uint32_t PRIM_POSTDIV = (5u << 16) | (5u << 12); // POSTDIV1=5, POSTDIV2=5
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H
