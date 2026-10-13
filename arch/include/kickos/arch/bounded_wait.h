// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A chip's register poll, bounded at KICKOS_POLL_SPIN_MAX reads: a dead crystal, an unlocked PLL
// or a wedged UART degrades to a false answer and never hangs the core. Inlined, so a poll on a
// panic path adds no frame to it.

#ifndef KICKOS_ARCH_BOUNDED_WAIT_H
#define KICKOS_ARCH_BOUNDED_WAIT_H

#include <kickos/config/limits.h>

#include <stdint.h>

namespace kickos::arch
{
    // True once (reg & mask) == want.
    template <typename T>
    __attribute__((always_inline)) inline bool wait_bits(T volatile& reg, T mask, T want)
    {
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
        {
            if ((reg & mask) == want)
            {
                return true;
            }
        }
        return false;
    }

    __attribute__((always_inline)) inline bool wait_set(uint32_t volatile& reg, uint32_t mask)
    {
        return wait_bits<uint32_t>(reg, mask, mask);
    }

    __attribute__((always_inline)) inline bool wait_set(uint8_t volatile& reg, uint8_t mask)
    {
        return wait_bits<uint8_t>(reg, mask, mask);
    }

    __attribute__((always_inline)) inline bool wait_clear(uint32_t volatile& reg, uint32_t mask)
    {
        return wait_bits<uint32_t>(reg, mask, 0u);
    }
}

#endif
