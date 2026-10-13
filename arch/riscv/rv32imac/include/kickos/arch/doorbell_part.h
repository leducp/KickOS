// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the shared doorbell protocol takes from THIS PART: the padding a cell owes, the spin a
// wait costs, and the tag each refusal names it by.

#ifndef KICKOS_ARCH_DOORBELL_PART_H
#define KICKOS_ARCH_DOORBELL_PART_H

#include <kickos/arch/arch.h>

// A cell per writer, padded to this size.
#define KICKOS_DOORBELL_LINE 64u

// Must equal the doorbell gate's backend argument: the gate greps the image for each refusal.
#define KICKOS_DOORBELL_TAG "rv32imac"

namespace kickos::doorbell
{
    inline void part_spin(void) { __asm volatile("nop" ::: "memory"); }
}

#endif
