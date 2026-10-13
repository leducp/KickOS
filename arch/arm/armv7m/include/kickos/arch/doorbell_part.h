// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, and the tag each refusal names it by. The RP2350 is the one armv7m part with a
// doorbell.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

// No row padding: the RP2350 caches only XIP, and SRAM0-7 stripe on address bits 3:2 (RP2350
// datasheet RP-008373-DS-2, 2.2.3), so adjacent cells already answer from different banks.
#define KICKOS_DOORBELL_LINE 4u

// Must equal the doorbell gate's backend argument: the gate greps the image for each refusal.
#define KICKOS_DOORBELL_TAG "armv7m"

namespace kickos::doorbell
{
    inline void part_spin(void)
    {
        __asm volatile("yield" ::: "memory");
    }
}

#endif
