// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 CLOCKS register map (RP2040 datasheet, RP-008371-DS, 2.15). Glitchless
// muxes (clk_ref, clk_sys) expose a one-hot SELECTED readback that must be polled
// after a SRC change; clk_peri is a plain aux mux with an enable bit.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_CLOCKS_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_CLOCKS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::clocks
{
    constexpr uintptr_t CLK_REF_CTRL = mmap::CLOCKS_BASE + 0x30u;
    constexpr uintptr_t CLK_REF_SELECTED = mmap::CLOCKS_BASE + 0x38u;
    constexpr uintptr_t CLK_SYS_CTRL = mmap::CLOCKS_BASE + 0x3cu;
    constexpr uintptr_t CLK_SYS_SELECTED = mmap::CLOCKS_BASE + 0x44u;
    constexpr uintptr_t CLK_PERI_CTRL = mmap::CLOCKS_BASE + 0x48u;

    constexpr uint32_t CLK_REF_SRC_XOSC = 0x2u;          // CTRL.SRC glitchless
    constexpr uint32_t CLK_REF_SELECTED_XOSC = 1u << 2;  // one-hot readback

    // clk_sys glitchless mux: SRC bit0 (0=clk_ref, 1=aux); AUXSRC[7:5]=0 selects
    // clksrc_pll_sys. SELECTED is one-hot on SRC (bit0=ref, bit1=aux).
    constexpr uint32_t CLK_SYS_AUXSRC_PLL = 0x0u << 5;
    constexpr uint32_t CLK_SYS_SRC_REF = 0x0u;
    constexpr uint32_t CLK_SYS_SRC_AUX = 0x1u;
    constexpr uint32_t CLK_SYS_SELECTED_REF = 1u << 0;
    constexpr uint32_t CLK_SYS_SELECTED_AUX = 1u << 1;
    constexpr uint32_t CLK_SYS_HZ = 125000000u; // clk_sys once on PLL_SYS

    // clk_peri: ENABLE(bit11) | AUXSRC. XOSC_CLKSRC=0x4 (12 MHz, used if the PLL
    // never locks); CLK_SYS=0x0 tracks clk_sys (125 MHz on PLL, ROSC in fallback).
    constexpr uint32_t CLK_PERI_ENABLE_XOSC = (1u << 11) | (0x4u << 5);
    constexpr uint32_t CLK_PERI_ENABLE_CLK_SYS = (1u << 11) | (0x0u << 5);
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_CLOCKS_H
