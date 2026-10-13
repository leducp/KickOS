// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_SWAP_H
#define KICKOS_ARCH_ATOMIC_SWAP_H

#include <stdint.h>

// Stores value at word and answers what it held, in one indivisible step, with acquire and
// release ordering.
inline uint32_t arch_swap_u32(uint32_t* word, uint32_t value)
{
    uint32_t old = 0;
    __asm volatile("amoswap.w.aqrl %0, %2, (%1)" : "=r"(old) : "r"(word), "r"(value) : "memory");
    return old;
}

#endif
