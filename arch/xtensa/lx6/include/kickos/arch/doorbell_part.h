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

// NO CACHE MEANS NO LINE TO SHARE, so a cell carries no padding. Do not widen this: the two
// caches serve the external path alone and kernel state is in internal SRAM.
#define KICKOS_DOORBELL_LINE 4u

namespace kickos::doorbell
{
    // Nothing to yield to on this core: Xtensa has no hint instruction a spin can take, and
    // WAITI would sleep a core that must keep polling.
    inline void part_spin(void)
    {
        __asm volatile("nop" ::: "memory");
    }

    // SYNC, not the buffered writer: a refusal fires from a spin that may be inside an
    // interrupt or under this core's own mask, where the thread-only buffered path does not
    // belong.
    inline void part_write(char const* buf, size_t n)
    {
        arch_console_write_sync(buf, n);
    }

    constexpr char PART_UNANSWERED[] = "KickOS: lx6 doorbell unanswered by core ";
    constexpr char PART_EARLY_WAIT[] = "KickOS: lx6 doorbell wait returned unanswered, rounds 0x";
    constexpr char PART_NO_CONTEND[] = "KickOS: lx6 kernel lock uncontended, peers ";
    constexpr char PART_NO_SPIN[]
        = "KickOS: lx6 no peer reached the acquire loop, spinning mask 0x";
    constexpr char PART_STILL_SETTLED[]
        = "KickOS: lx6 doorbell round settled a request that never moved, peers ";
}

#endif
