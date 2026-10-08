// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The address-space boundary of the K-seam for the range-check gate at KICKOS_HAVE_ASPACE=1.
// kernel/domain is not compiled here; what this stands in for is the list a domain holds and
// the space that makes it reachable, which is what the range checks ask on a translating
// backend.
//
// Keep this header GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_RANGECHECK_ASPACE_SEAM_H
#define KICKOS_TESTS_UNIT_RANGECHECK_ASPACE_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Domain;
    class VirtualRanges;

    namespace testfix
    {
        // Give `d` a space of its own and an empty list keyed on the arch granule, so
        // domain_ranges answers it. Null where `d` names no pool slot.
        VirtualRanges* seat_space(Domain* d);

        // Take the space away and leave the list behind. domain_ranges answers null after
        // this, which is the shape of a domain that holds no space at all.
        void drop_space(Domain* d);

        // What arch_aspace_acquire answers in any space: [va, va + bytes) reached at `at`, the
        // pages from `uncached_va` on reported mapped non-cacheable. Zero bytes backs nothing.
        struct Backing
        {
            uintptr_t va;
            unsigned char* at;
            size_t bytes;
            uintptr_t uncached_va;
        };
        extern Backing g_backing;
        void seat_backing(uintptr_t va, unsigned char* at, size_t bytes, uintptr_t uncached_va);
    }
}

#endif
