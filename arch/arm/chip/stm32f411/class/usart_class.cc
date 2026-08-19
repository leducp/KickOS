// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "usart_class.h"

#include <regs/usart.h> // chip register offsets + bit fields

namespace kickos::stm32f411::driver
{
    bool usart_tx_ready(uintptr_t base)
    {
        volatile uint32_t const& sr =
            *reinterpret_cast<volatile uint32_t const*>(base + reg::usart::SR_OFFSET);
        return (sr & reg::usart::SR_TXE) != 0u;
    }
}
