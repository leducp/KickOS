// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, the console a refusal reaches, and the wording of each refusal.
//
// THE WORDING IS THE PART'S. tests/integration/check_smp_doorbell.sh greps each refusal as this
// backend's own literal, and one spelling shared across backends would leave its arms grepping
// an image for lines that image cannot print.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

#include <stddef.h>

// A53 cache line.
#define KICKOS_DOORBELL_LINE 64u

namespace kickos::doorbell
{
    inline void part_spin(void)
    {
        __asm volatile("yield" ::: "memory");
    }

    inline void part_write(char const* buf, size_t n)
    {
        arch_console_write(buf, n);
    }

    constexpr char PART_UNANSWERED[] = "KickOS: armv8a doorbell unanswered by core ";
    constexpr char PART_EARLY_WAIT[]
        = "KickOS: armv8a doorbell wait returned unanswered, rounds 0x";
    constexpr char PART_NO_CONTEND[] = "KickOS: armv8a kernel lock uncontended, peers ";
    constexpr char PART_NO_SPIN[]
        = "KickOS: armv8a no peer reached the acquire loop, spinning mask 0x";
    constexpr char PART_STILL_SETTLED[]
        = "KickOS: armv8a doorbell round settled a request that never moved, peers ";
}

#endif
