// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The STM32 USART baud divisor.

#ifndef KICKOS_ARCH_ARM_COMMON_STM32_USART_H
#define KICKOS_ARCH_ARM_COMMON_STM32_USART_H

#include <stdint.h>

namespace kickos::stm32
{
    // OVER8=0 -> BRR = round(fck / baud).
    constexpr uint32_t usart_brr(uint32_t fck, uint32_t baud)
    {
        return (fck + baud / 2u) / baud;
    }
}

#endif
