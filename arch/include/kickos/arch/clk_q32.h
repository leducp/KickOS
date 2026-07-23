// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Single source for the tickless-clock Q32.32 fixed-point math shared by every
// chip's arch_clock_now. A tick rate is turned into a Q32 reciprocal once, then
// ticks->ns is a 64x64->64 multiply with no per-read divide.

#ifndef KICKOS_ARCH_CLK_Q32_H
#define KICKOS_ARCH_CLK_Q32_H

#include <stdint.h>

#include <kickos/units.h> // _s literal -> the canonical 1e9 ns/sec constant

namespace kickos
{
    // Canonical nanoseconds per second. Sole spelling of the bare 1e9 constant.
    constexpr uint64_t KICKOS_NS_PER_SEC = 1_s;

    // Q32.32 reciprocal of a tick rate: (1e9 << 32) / hz, rounded to nearest.
    // The `+ (hz >> 1)` bias is LOAD-BEARING: it is the round-to-nearest term;
    // without it every ticks->ns conversion is biased low by up to one ns.
    // Caller must ensure hz != 0.
    constexpr uint64_t arch_clk_recip_q32(uint64_t hz)
    {
        return ((KICKOS_NS_PER_SEC << 32) + (hz >> 1)) / hz;
    }

    // (ticks * mult_q32) >> 32, split hi/lo so the full product never overflows
    // 64 bits. Returns ns (the caller adds any epoch base).
    constexpr uint64_t arch_clk_mul_q32(uint64_t ticks, uint64_t mult_q32)
    {
        uint64_t a = ticks >> 32, b = ticks & 0xFFFFFFFFull;
        uint64_t c = mult_q32 >> 32, d = mult_q32 & 0xFFFFFFFFull;
        return ((a * c) << 32) + a * d + b * c + ((b * d) >> 32);
    }
}

#endif
