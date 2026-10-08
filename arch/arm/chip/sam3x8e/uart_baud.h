// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SAM3X8E UART baud divisor (SAM3X/SAM3A datasheet sec.34, UART_BRGR).

#ifndef KICKOS_ARCH_ARM_CHIP_SAM3X8E_UART_BAUD_H
#define KICKOS_ARCH_ARM_CHIP_SAM3X8E_UART_BAUD_H

#include <stdint.h>

namespace kickos::sam3x8e
{
    // CD = MCK/(16*baud), rounded. CD 0 stops the generator, so a clock too slow to divide
    // gets 1 rather than silence.
    constexpr uint32_t uart_brgr_cd(uint32_t mck, uint32_t baud)
    {
        uint32_t const div = 16u * baud;
        uint32_t const cd = (mck + div / 2u) / div;
        if (cd == 0u)
        {
            return 1u;
        }
        return cd;
    }
}

#endif
