// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ATOMIC_CAS_BOUNDED_H
#define KICKOS_ARCH_ATOMIC_CAS_BOUNDED_H

#include <stdint.h>

// arch_cas_u32 for an arch whose store can be lost, over the arch's arch_cas_u32_once: one load
// and, when it matches `want`, one store attempt, answering whether the store landed, with
// *observed what was loaded.
bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed);

// At most this many stores under the caller's lock; a lost run is no error, the caller retries
// with its lock dropped.
inline constexpr uint32_t ARCH_CAS_U32_STORES = 8u;

// Stores desired at word if it holds *expected, with acquire and release ordering, and answers
// whether it did; when it did not, *expected takes what the word held, which is *expected
// itself when every store was lost.
inline bool arch_cas_u32(uint32_t* word, uint32_t* expected, uint32_t desired)
{
    uint32_t const want = *expected;
    uint32_t observed = want;
    for (uint32_t i = 0; i < ARCH_CAS_U32_STORES; i++)
    {
        if (arch_cas_u32_once(word, want, desired, &observed))
        {
            return true;
        }
        if (observed != want)
        {
            break;
        }
    }
    *expected = observed;
    return false;
}

#endif
