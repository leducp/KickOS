// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 IOMUXC pin-mux register map (RM ch.11): the SW_MUX_CTL_PAD_* mux
// selects, the SW_PAD_CTL_PAD_* pad controls, and the *_SELECT_INPUT daisy-chain registers
// that pick which pad drives a peripheral input (RM 11.3).

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_IOMUXC_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::iomuxc
{
    // SW_MUX_CTL_PAD registers, one word per pad: GPIO_AD_B0_00 (GPIO1.IO00) at 0x0BC and
    // GPIO_B0_00 (GPIO2.IO00) at 0x13C.
    constexpr uintptr_t SW_MUX_AD_B0_00 = mmap::IOMUXC_BASE + 0xBCu;
    constexpr uintptr_t SW_MUX_B0_00 = mmap::IOMUXC_BASE + 0x13Cu;

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

    // SW_PAD_CTL_PAD registers, laid out as the mux ones: GPIO_AD_B0_00 at 0x2AC and GPIO_B0_00 at
    // 0x32C (RM 11.6.167, 11.6.199).
    constexpr uintptr_t SW_PAD_AD_B0_00 = mmap::IOMUXC_BASE + 0x2ACu;
    constexpr uintptr_t SW_PAD_B0_00 = mmap::IOMUXC_BASE + 0x32Cu;

    constexpr uintptr_t sw_pad(uint32_t bank, uint32_t bit)
    {
        if (bank == 1u)
        {
            return SW_PAD_AD_B0_00 + bit * 4u;
        }
        return SW_PAD_B0_00 + bit * 4u;
    }

    // The pads' reset value: push-pull, drive R0/6, keeper enabled (RM 11.6.199).
    constexpr uint32_t PAD_CTL_RESET = 0x10B0u;

    // Daisy-chain input selects: which pad feeds an LPUART's RX (RM 11.6.327 to 11.6.340,
    // pp.848-858). LPUART1's RX has one pad and no select.
    constexpr uintptr_t LPUART2_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x52Cu;
    constexpr uintptr_t LPUART3_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x538u;
    constexpr uintptr_t LPUART4_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x540u;
    constexpr uintptr_t LPUART5_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x548u;
    constexpr uintptr_t LPUART6_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x550u;
    constexpr uintptr_t LPUART7_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x558u;
    constexpr uintptr_t LPUART8_RX_SELECT_INPUT = mmap::IOMUXC_BASE + 0x560u;

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
