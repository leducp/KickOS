// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The page-table editor for a host gate over a translating kernel: the arch_aspace_* boundary
// over the host frame pool (host_frame_pool.h), one leaf per mapped granule. A user address is
// its frame's pool address. Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_COMMON_HOST_ASPACE_H
#define KICKOS_TESTS_UNIT_COMMON_HOST_ASPACE_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        // Each space takes its root, and a table per 1 GiB and per 2 MiB of address it maps,
        // from the pool, and keeps the tables a refused map took until it is destroyed.
        void host_aspace_tables_from_pool(bool on);

        // Spaces created and not yet destroyed, and destroys of a space already destroyed.
        extern size_t g_aspaces_live;
        extern size_t g_aspace_double_destroys;

        // arch_aspace_unmap calls that unmapped, and the pages they unmapped.
        extern uint32_t g_unmaps;
        extern size_t g_unmapped_pages;

        // The `count` acquires of the page holding `va` that follow the next `after` answer
        // null, in any space, as they would once the page is unmapped.
        void refuse_acquire(uintptr_t va, uint32_t count, uint32_t after = 0);

        // Every map answers ENOMEM while set.
        void refuse_map(bool on);

        // Whether the page holding `va` was still mapped when its space was last destroyed.
        void watch_destroy(uintptr_t va);
        extern bool g_watched_at_destroy;

        // Clears the counters and the refusals. The spaces keep what they map.
        void host_aspace_reset();
    }
}

#endif
