// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, and the tag each refusal names it by.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

// NO CACHE MEANS NO LINE TO SHARE, so a cell carries no padding. Do not widen this: the two
// caches serve the external path alone and kernel state is in internal SRAM.
#define KICKOS_DOORBELL_LINE 4u

// Must equal the doorbell gate's backend argument: the gate greps the image for each refusal.
#define KICKOS_DOORBELL_TAG "lx6"

namespace kickos::doorbell
{
    // Nothing to yield to on this core: Xtensa has no hint instruction a spin can take, and
    // WAITI would sleep a core that must keep polling.
    inline void part_spin(void)
    {
        __asm volatile("nop" ::: "memory");
    }
}

#endif
