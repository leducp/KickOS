// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 MCG register map (K64 Sub-Family RM ch.25). 8-bit registers.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_MCG_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_MCG_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::mk64f::reg::mcg
{
    constexpr uintptr_t C1 = mmap::MCG_BASE + 0x00u;
    constexpr uintptr_t C2 = mmap::MCG_BASE + 0x01u;
    constexpr uintptr_t C5 = mmap::MCG_BASE + 0x04u;
    constexpr uintptr_t C6 = mmap::MCG_BASE + 0x05u;
    constexpr uintptr_t S = mmap::MCG_BASE + 0x06u;

    constexpr uint8_t C2_RANGE_VHF = 2u << 4; // RANGE0=2; EREFS0=0 => external clock (not xtal)
    constexpr uint8_t C1_CLKS_EXT = 2u << 6;  // CLKS=2 external reference
    constexpr uint8_t C1_CLKS_PLL = 0u << 6;  // CLKS=0 FLL/PLL output (PLL, since PLLS=1)
    constexpr uint8_t C1_IREFS_INT = 1u << 2; // IREFS=1 internal ref (FEI posture, CLKS=0)
    constexpr uint8_t C1_FRDIV_1536 = 7u << 3; // RANGE!=0: /1536 -> 50 MHz FLL ref = 32.6 kHz
    constexpr uint8_t C5_PRDIV_20 = 19u;       // (PRDIV0+1)=20 -> PLL ref 50/20 = 2.5 MHz
    constexpr uint8_t C6_PLLS = 1u << 6;
    constexpr uint8_t C6_VDIV_48 = 24u; // (VDIV0+24)=48 -> VCO 2.5*48 = 120 MHz

    constexpr uint8_t S_IREFST = 1u << 4;    // 0 = external ref selected
    constexpr uint8_t S_CLKST_MASK = 3u << 2;
    constexpr uint8_t S_CLKST_EXT = 2u << 2; // MCGOUTCLK = external ref
    constexpr uint8_t S_CLKST_PLL = 3u << 2; // MCGOUTCLK = PLL
    constexpr uint8_t S_PLLST = 1u << 5;     // PLL (not FLL) is the PLLS source
    constexpr uint8_t S_LOCK0 = 1u << 6;
}

#endif
