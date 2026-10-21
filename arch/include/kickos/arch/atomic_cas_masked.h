// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_MASKED_H
#define KICKOS_ARCH_ATOMIC_CAS_MASKED_H

#include <kickos/arch/arch.h>

#include <stdint.h>

// arch_cas_u32 for a kernel that runs on one core: a load, a compare and a store with
// interrupts masked. Atomic against a user thread only because a user compare-and-swap
// switched out between its load and its store fails that store, and only while no interrupt
// handler writes the word.
inline bool arch_cas_u32(uint32_t* word, uint32_t* expected, uint32_t desired)
{
    uint32_t volatile* const w = word;
    arch_irq_state_t const s = arch_irq_save();
    uint32_t const observed = *w;
    bool const stored = observed == *expected;
    if (stored)
    {
        *w = desired;
    }
    arch_irq_restore(s);
    if (not stored)
    {
        *expected = observed;
    }
    return stored;
}

#endif
