// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 IOMUXC pin-mux register map (RM ch.11). Two register families
// are involved: the SW_MUX_CTL_PAD_* mux selects, and the *_SELECT_INPUT daisy-
// chain registers that pick which pad drives a peripheral input (RM 11.3). Only a
// PARTIAL set of pads is named here: the LPUART6 console pads plus a handful of
// common GPIO1/GPIO2 pads the pinmux exercise needs. The full 124-pad
// SW_MUX block and the SW_PAD_CTL / GPIO data blocks are intentionally NOT authored;
// arch_pinmux_set hard-fails EINVAL on any pad not in its table, so a hole is loud.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::imxrt1062::reg::iomuxc
{
    // SW_MUX_CTL_PAD registers. The block runs GPIO_EMC_00 (0x014) contiguously; the
    // GPIO_AD_B0_xx pads start at 0x0BC and GPIO_B0_xx at 0x13C (each +4). Named here:
    // GPIO1.IO00..05 (= GPIO_AD_B0_00..05) and GPIO2.IO00..03 (= GPIO_B0_00..03).
    constexpr uintptr_t SW_MUX_AD_B0_00 = mmap::IOMUXC_BASE + 0xBCu; // GPIO1.IO00
    constexpr uintptr_t SW_MUX_AD_B0_01 = mmap::IOMUXC_BASE + 0xC0u; // GPIO1.IO01
    constexpr uintptr_t SW_MUX_AD_B0_02 = mmap::IOMUXC_BASE + 0xC4u; // GPIO1.IO02 = Teensy pin 1 = TX1
    constexpr uintptr_t SW_MUX_AD_B0_03 = mmap::IOMUXC_BASE + 0xC8u; // GPIO1.IO03 = Teensy pin 0 = RX1
    constexpr uintptr_t SW_MUX_AD_B0_04 = mmap::IOMUXC_BASE + 0xCCu; // GPIO1.IO04
    constexpr uintptr_t SW_MUX_AD_B0_05 = mmap::IOMUXC_BASE + 0xD0u; // GPIO1.IO05
    constexpr uintptr_t SW_MUX_B0_00 = mmap::IOMUXC_BASE + 0x13Cu;   // GPIO2.IO00
    constexpr uintptr_t SW_MUX_B0_01 = mmap::IOMUXC_BASE + 0x140u;   // GPIO2.IO01
    constexpr uintptr_t SW_MUX_B0_02 = mmap::IOMUXC_BASE + 0x144u;   // GPIO2.IO02
    constexpr uintptr_t SW_MUX_B0_03 = mmap::IOMUXC_BASE + 0x148u;   // GPIO2.IO03 = Teensy pin 13 (LED)

    // Daisy-chain input select: which pad feeds LPUART6_RX (RM ch.11).
    constexpr uintptr_t LPUART6_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x4E4u;

    // SW_MUX_CTL_PAD field encoding.
    constexpr uint32_t MUX_MODE_MASK = 0x7u;  // MUX_MODE[3:0] (only 0..7 defined per pad)
    constexpr uint32_t SION_BIT = 1u << 4;    // software-input-on
    constexpr uint32_t MUX_FIELD_MASK = 0x1Fu; // MUX_MODE | SION, the bits arch_pinmux_set writes

    constexpr uint32_t MUX_ALT2 = 2u;          // ALT2 = LPUART6_TX / _RX
    constexpr uint32_t RX_DAISY_AD_B0_03 = 1u; // daisy value selecting AD_B0_03
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
