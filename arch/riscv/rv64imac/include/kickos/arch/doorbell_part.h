// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, and the tag each refusal names it by.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

// Chosen: RISC-V publishes no cache-line width in any CSR.
#define KICKOS_DOORBELL_LINE 64u

// Must equal the doorbell gate's backend argument: the gate greps the image for each refusal.
#define KICKOS_DOORBELL_TAG "rv64imac"

namespace kickos::doorbell
{
    // No hint instruction in this baseline: Zihintpause is not in the board's ISA string.
    inline void part_spin(void)
    {
        __asm volatile("nop" ::: "memory");
    }
}

#endif
