// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 IOMUXC pin-mux register map (RM ch.11). Only the pads KickOS
// routes for the LPUART6 console (Teensy pins 0/1). Two register families are
// involved: the SW_MUX_CTL_PAD_* mux selects, and the *_SELECT_INPUT daisy-chain
// registers that pick which pad drives a peripheral input (RM 11.3).

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::imxrt1062::reg::iomuxc
{
    // Pad mux-control registers.
    constexpr uintptr_t SW_MUX_AD_B0_02 = mmap::IOMUXC_BASE + 0xC4u; // Teensy pin 1 = TX1
    constexpr uintptr_t SW_MUX_AD_B0_03 = mmap::IOMUXC_BASE + 0xC8u; // Teensy pin 0 = RX1

    // Daisy-chain input select: which pad feeds LPUART6_RX (RM ch.11).
    constexpr uintptr_t LPUART6_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x4E4u;

    constexpr uint32_t MUX_ALT2 = 2u;          // ALT2 = LPUART6_TX / _RX
    constexpr uint32_t RX_DAISY_AD_B0_03 = 1u; // daisy value selecting AD_B0_03
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
