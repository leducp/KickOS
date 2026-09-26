// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

#include <stddef.h>

#define KICKOS_DOORBELL_LINE 64u

namespace kickos::doorbell
{
    inline void part_spin(void)
    {
        __asm__ volatile("pause" ::: "memory");
    }

    inline void part_write(char const* buf, size_t n)
    {
        arch_console_write(buf, n);
    }

    constexpr char PART_UNANSWERED[] = "KickOS: x86_64 doorbell unanswered by core ";
    constexpr char PART_EARLY_WAIT[]
        = "KickOS: x86_64 doorbell wait returned unanswered, rounds 0x";
    constexpr char PART_NO_CONTEND[] = "KickOS: x86_64 kernel lock uncontended, peers ";
    constexpr char PART_NO_SPIN[]
        = "KickOS: x86_64 no peer reached the acquire loop, spinning mask 0x";
    constexpr char PART_STILL_SETTLED[]
        = "KickOS: x86_64 doorbell round settled a request that never moved, peers ";
}

#endif
