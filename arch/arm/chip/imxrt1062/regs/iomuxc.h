// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 IOMUXC pin-mux register map (RM ch.11). Two register families
// are involved: the SW_MUX_CTL_PAD_* mux selects, and the *_SELECT_INPUT daisy-
// chain registers that pick which pad drives a peripheral input (RM 11.3). sw_mux()
// addresses every pad of GPIO banks 1 and 2. The pads named below are the ones
// arch_pinmux_set's table lists, and that table is the partial part: it hard-fails
// EINVAL on any pad it does not list, so a hole is loud. The SW_PAD_CTL and GPIO data
// blocks are not authored.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::iomuxc
{
    // SW_MUX_CTL_PAD registers. The block runs GPIO_EMC_00 (0x014) contiguously; the
    // GPIO_AD_B0_xx pads start at 0x0BC and GPIO_B0_xx at 0x13C (each +4). Named here:
    // GPIO1.IO00..05 (= GPIO_AD_B0_00..05) and GPIO2.IO00..03 (= GPIO_B0_00..03).
    constexpr uintptr_t SW_MUX_AD_B0_00 = mmap::IOMUXC_BASE + 0xBCu; // GPIO1.IO00
    constexpr uintptr_t SW_MUX_AD_B0_01 = mmap::IOMUXC_BASE + 0xC0u; // GPIO1.IO01
    constexpr uintptr_t SW_MUX_AD_B0_02 = mmap::IOMUXC_BASE + 0xC4u; // GPIO1.IO02
    constexpr uintptr_t SW_MUX_AD_B0_03 = mmap::IOMUXC_BASE + 0xC8u; // GPIO1.IO03
    constexpr uintptr_t SW_MUX_AD_B0_04 = mmap::IOMUXC_BASE + 0xCCu; // GPIO1.IO04
    constexpr uintptr_t SW_MUX_AD_B0_05 = mmap::IOMUXC_BASE + 0xD0u; // GPIO1.IO05
    constexpr uintptr_t SW_MUX_B0_00 = mmap::IOMUXC_BASE + 0x13Cu;   // GPIO2.IO00
    constexpr uintptr_t SW_MUX_B0_01 = mmap::IOMUXC_BASE + 0x140u;   // GPIO2.IO01
    constexpr uintptr_t SW_MUX_B0_02 = mmap::IOMUXC_BASE + 0x144u;   // GPIO2.IO02
    constexpr uintptr_t SW_MUX_B0_03 = mmap::IOMUXC_BASE + 0x148u;   // GPIO2.IO03

    // The SW_MUX_CTL_PAD of GPIO bank 1's or 2's pad `bit`: bank 1 is GPIO_AD_B0_00 onwards and
    // bank 2 GPIO_B0_00 onwards, each B1 block following its B0 block without a gap.
    constexpr uintptr_t sw_mux(uint32_t bank, uint32_t bit)
    {
        if (bank == 1u)
        {
            return SW_MUX_AD_B0_00 + bit * 4u;
        }
        return SW_MUX_B0_00 + bit * 4u;
    }

    // Daisy-chain input select: which pad feeds LPUART6_RX (RM 11.6.336, p.855).
    constexpr uintptr_t LPUART6_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x550u;

    // GPR27 (RM 11.3.28), in the SEPARATE IOMUXC_GPR block. Bit n picks which instance owns
    // pad n of the shared GPIO2/GPIO7 pair: 0 = GPIO2, 1 = GPIO7. Resets to 0.
    constexpr uintptr_t GPR27 = mmap::IOMUXC_GPR_BASE + 0x6Cu;

    // SW_MUX_CTL_PAD field encoding.
    constexpr uint32_t MUX_MODE_MASK = 0x7u;  // MUX_MODE[3:0] (only 0..7 defined per pad)
    constexpr uint32_t SION_BIT = 1u << 4;    // software-input-on
    constexpr uint32_t MUX_FIELD_MASK = 0x1Fu; // MUX_MODE | SION, the bits arch_pinmux_set writes

    constexpr uint32_t MUX_ALT5 = 5u; // ALT5 = GPIO, on every pad of banks 1 and 2
}

#endif
