// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Q32.32 tick-to-ns math of every chip's arch_clock_now: the rate becomes a reciprocal once,
// and a read is a multiply with no divide.

#ifndef KICKOS_ARCH_CLK_Q32_H
#define KICKOS_ARCH_CLK_Q32_H

#include <stdint.h>

#include <kickos/units.h> // _s literal -> the canonical 1e9 ns/sec constant

namespace kickos
{
    // Canonical nanoseconds per second. Sole spelling of the bare 1e9 constant.
    constexpr uint64_t KICKOS_NS_PER_SEC = 1_s;

    // (1e9 << 32) / hz, rounded to nearest by the `+ (hz >> 1)` term: without it every
    // conversion is biased low by up to one ns. hz must be nonzero.
    constexpr uint64_t arch_clk_recip_q32(uint64_t hz)
    {
        return ((KICKOS_NS_PER_SEC << 32) + (hz >> 1)) / hz;
    }

    // (ticks * mult_q32) >> 32, split hi/lo so the product never overflows 64 bits.
    constexpr uint64_t arch_clk_mul_q32(uint64_t ticks, uint64_t mult_q32)
    {
        uint64_t a = ticks >> 32, b = ticks & 0xFFFFFFFFull;
        uint64_t c = mult_q32 >> 32, d = mult_q32 & 0xFFFFFFFFull;
        return ((a * c) << 32) + a * d + b * c + ((b * d) >> 32);
    }
}

#endif
