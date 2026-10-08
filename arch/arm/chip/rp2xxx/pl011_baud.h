// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PL011 baud divisor both RP2xxx parts program (RP2040 DS 4.2.3.2, RP2350 DS 12.1.3.2):
// baud = clk_peri / (16 x (IBRD + FBRD/64)), FBRD = round(fraction x 64).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_PL011_BAUD_H
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_PL011_BAUD_H

#include <stdint.h>

namespace kickos::rp2xxx
{
    // The divisor in 64ths, rounded once, so a fraction that rounds up to 64 carries into
    // IBRD instead of overflowing FBRD's six bits.
    constexpr uint32_t pl011_div64(uint32_t clk, uint32_t baud)
    {
        return (4u * clk + baud / 2u) / baud;
    }

    constexpr uint32_t pl011_ibrd(uint32_t clk, uint32_t baud)
    {
        return pl011_div64(clk, baud) / 64u;
    }

    constexpr uint32_t pl011_fbrd(uint32_t clk, uint32_t baud)
    {
        return pl011_div64(clk, baud) % 64u;
    }
}

#endif
