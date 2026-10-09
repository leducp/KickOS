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

#include "acquire_holds.h"

struct arch_aspace;

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

        // What arch_aspace_acquire answers, a page at a time: the page at `va` in `space`, or in
        // every space where it is null, reached at `at`. False, backing nothing, for an unaligned
        // `va` or past BACKED_PAGES_MAX pages.
        constexpr size_t BACKED_PAGES_MAX = 8;
        [[nodiscard]] bool back_page(struct arch_aspace const* space, uintptr_t va,
                                     unsigned char* at, bool uncached);
        // [va, va + bytes) reached at `at` in every space, the pages from `uncached_va` on
        // reported mapped non-cacheable.
        [[nodiscard]] bool seat_backing(uintptr_t va, unsigned char* at, size_t bytes,
                                        uintptr_t uncached_va);
        // Backs nothing, and forgets every hold.
        void unback_all();

        AcquireHolds const& holds();
        // The acquires refused because ARCH_ASPACE_ACQUIRE_MIN were already live.
        size_t holds_refused_past_min();
    }
}

#endif
