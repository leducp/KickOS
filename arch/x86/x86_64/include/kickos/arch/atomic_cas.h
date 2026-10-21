// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_H
#define KICKOS_ARCH_ATOMIC_CAS_H

#include <stdint.h>

// Stores desired at word if it holds *expected, with acquire and release ordering, and answers
// whether it did; when it did not, *expected takes what the word held.
inline bool arch_cas_u32(uint32_t* word, uint32_t* expected, uint32_t desired)
{
    bool stored = false;
    __asm__ volatile("lock cmpxchgl %3, %1"
                     : "+a"(*expected), "+m"(*word), "=@ccz"(stored)
                     : "r"(desired)
                     : "memory");
    return stored;
}

#endif
