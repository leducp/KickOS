// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The GPIO register shapes the STM32 F1, F3 and F4 parts share: a pair of words with four bits
// per pin (CRL/CRH, AFRL/AFRH), and BSRR's set and reset halves.

#ifndef KICKOS_ARCH_ARM_COMMON_STM32_GPIO_H
#define KICKOS_ARCH_ARM_COMMON_STM32_GPIO_H

#include "regs.h" // arch/arm/common: kickos::arm::reg32

#include <stdint.h>

namespace kickos::stm32
{
    constexpr uintptr_t nibble_reg(uintptr_t low, uint32_t pin)
    {
        if (pin < 8u)
        {
            return low;
        }
        return low + 4u;
    }

    constexpr uint32_t nibble_shift(uint32_t pin)
    {
        return (pin % 8u) * 4u;
    }

    constexpr uint32_t bsrr_word(uint32_t pin, bool level)
    {
        if (level)
        {
            return 1u << pin;
        }
        return 1u << (pin + 16u);
    }

    inline void rmw(uintptr_t addr, uint32_t clear, uint32_t set)
    {
        uint32_t v = arm::reg32(addr);
        v &= ~clear;
        v |= set;
        arm::reg32(addr) = v;
    }
}

#endif
