// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 CLOCKS register map (RP2350 datasheet RP-008373-DS-2, 8.1): glitchless
// clock muxes for clk_ref / clk_sys / clk_peri. Offsets are CLOCKS_BASE-relative
// (see mmap.h). SELECTED registers read back one-hot on the SRC field.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_CLOCKS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_CLOCKS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2350::reg::clocks
{
    constexpr uintptr_t CLK_REF_CTRL = mmap::CLOCKS_BASE + 0x30u;
    constexpr uintptr_t CLK_REF_SELECTED = mmap::CLOCKS_BASE + 0x38u;
    constexpr uintptr_t CLK_SYS_CTRL = mmap::CLOCKS_BASE + 0x3cu;
    constexpr uintptr_t CLK_SYS_SELECTED = mmap::CLOCKS_BASE + 0x44u;
    constexpr uintptr_t CLK_PERI_CTRL = mmap::CLOCKS_BASE + 0x48u;

    // clk_ref: SRC glitchless mux; SELECTED is one-hot (1 << SRC).
    constexpr uint32_t CLK_REF_SRC_XOSC = 0x2u;
    constexpr uint32_t CLK_REF_SELECTED_XOSC = 1u << 2;

    // clk_sys glitchless mux: SRC bit0 (0=clk_ref, 1=aux); AUXSRC[7:5]=0 selects
    // clksrc_pll_sys. SELECTED is one-hot on SRC (bit0=ref, bit1=aux).
    constexpr uint32_t CLK_SYS_AUXSRC_PLL = 0x0u << 5;
    constexpr uint32_t CLK_SYS_SRC_REF = 0x0u;
    constexpr uint32_t CLK_SYS_SRC_AUX = 0x1u;
    constexpr uint32_t CLK_SYS_SELECTED_REF = 1u << 0;
    constexpr uint32_t CLK_SYS_SELECTED_AUX = 1u << 1;

    // clk_peri: ENABLE(bit11) | AUXSRC[7:5]. XOSC=0x4 (12 MHz, PLL-never-locks
    // fallback); CLK_SYS=0x0 tracks clk_sys (150 MHz on PLL, 12 MHz clk_ref else).
    constexpr uint32_t CLK_PERI_ENABLE_XOSC = (1u << 11) | (0x4u << 5);
    constexpr uint32_t CLK_PERI_ENABLE_CLK_SYS = (1u << 11) | (0x0u << 5);

    // clk_sys target once on PLL_SYS (DS default max, 8.6).
    constexpr uint32_t CLK_SYS_HZ = 150000000u;
    // ROSC reset frequency (~6.5 MHz, uncalibrated); approximate SysTick timing if
    // the crystal never comes up.
    constexpr uint32_t ROSC_NOMINAL_HZ = 6500000u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_CLOCKS_H
