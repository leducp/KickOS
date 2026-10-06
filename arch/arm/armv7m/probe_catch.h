// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ARM_ARMV7M_PROBE_CATCH_H
#define KICKOS_ARCH_ARM_ARMV7M_PROBE_CATCH_H

#include <stdint.h>

namespace kickos::armv7m
{
    // Whether a fault at `pc` is the probe read's own: its load, while the read is armed, taken in
    // handler mode (EXC_RETURN bit 3 clear) or in privileged thread mode (CONTROL bit 0 clear).
    inline constexpr bool probe_claims(uint32_t pc, uint32_t load, bool armed, uint32_t exc_return,
                                       uint32_t control)
    {
        if (not armed or (pc & ~1u) != load)
        {
            return false;
        }
        return (exc_return & (1u << 3)) == 0u or (control & 1u) == 0u;
    }

    // Volatile: the fault path reads and writes it between the probe read's two stores to `armed`.
    struct ProbeLatch
    {
        volatile bool armed;
        volatile uint32_t cfsr;
        volatile uint32_t bfar;
    };

    // 0 with the word `load_word` read at `at` in `*value`, or the CFSR of the fault it took
    // with its BFAR, zero where the fault left none valid, in `*value`.
    inline uint32_t probe_read(ProbeLatch& latch, uint32_t (*load_word)(uintptr_t), uintptr_t at,
                               uint32_t* value)
    {
        latch.cfsr = 0u;
        latch.armed = true;
        uint32_t const word = load_word(at);
        latch.armed = false;
        uint32_t const cfsr = latch.cfsr;
        *value = word;
        if (cfsr != 0u)
        {
            *value = latch.bfar;
        }
        return cfsr;
    }

    // The fault path's half: claims a fault whose stacked `frame` is the armed probe's load at
    // `load`, records `cfsr` and `bfar`, and resumes past the 2-byte load.
    inline bool probe_catch(ProbeLatch& latch, uint32_t* frame, uint32_t load, uint32_t exc_return,
                            uint32_t control, uint32_t cfsr, uint32_t bfar)
    {
        if (not probe_claims(frame[6], load, latch.armed, exc_return, control))
        {
            return false;
        }
        latch.bfar = 0u;
        if (cfsr & (1u << 15))
        {
            latch.bfar = bfar;
        }
        latch.cfsr = cfsr;
        frame[6] = load + 2u;
        return true;
    }
}

#endif
