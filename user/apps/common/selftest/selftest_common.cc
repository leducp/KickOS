// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The definitions every self-test translation unit reaches.

#include "selftest.h"

namespace selftest
{
    kos_cap_t g_done = KOS_CAP_NONE; // shared completion counter (MAIN's cap; delegated to workers)
    kos_cap_t g_lock = KOS_CAP_NONE; // binary semaphore = mutex over the event log (MAIN's cap)

    void wait_n(int n)
    {
        for (int i = 0; i < n; i++)
        {
            kos_sem_wait(g_done);
        }
    }

    // The arena's allocation granule, or 0 where it cannot be established.
    // Memoised: on a bump arena kos_ram_alloc never frees, and on a 16 KiB part the arena
    // must stay whole for mem_self_grant to reach the region-descriptor ceiling.
    size_t g_granule = 0;

    size_t discover_granule()
    {
        if (g_granule != 0)
        {
            return g_granule;
        }
#if KICKOS_HAVE_ASPACE
        // ASKED, NOT MEASURED. Under translation kos_ram_alloc reserves frames out of a
        // first-fit bitmap, so the distance between two consecutive results is whatever the
        // holes left by earlier frees make it, and it can be zero or negative. Allocation
        // ORDER IS NOT PUBLIC API on this backend and no arm may derive geometry from it.
        // The power-of-two test rejects an error return, every granule being one.
        uint64_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        if (g == 0 or (g & (g - 1u)) != 0)
        {
            return 0;
        }
        g_granule = static_cast<size_t>(g);
        return g_granule;
#else
        // A bump arena: consecutive results ARE a stride, and there is no probe syscall on a
        // board that builds no selftest kernel half.
        void* p = kos_ram_alloc(1);
        void* q = kos_ram_alloc(1);
        if (p == nullptr or q == nullptr)
        {
            return 0;
        }
        uintptr_t const a = reinterpret_cast<uintptr_t>(p);
        uintptr_t const b = reinterpret_cast<uintptr_t>(q);
        if (b <= a)
        {
            return 0;
        }
        g_granule = static_cast<size_t>(b - a);
        return g_granule;
#endif
    }

    // Nothing waits on this probe, so any subset of a probe batch still drains.
    void pool_probe_worker(void*)
    {
        kos_sem_post(CH_DONE);
    }

    // Can this board host `n` workers CONCURRENTLY, right now? Slots held by service-list
    // drivers and arena room for each stack bound this as much as KICKOS_MAX_THREADS does.
    // Call immediately before the real spawns; when wait_n returns every probe slot is
    // EXITED and every probe stack is back on the free list.
    bool pool_can_host(int n)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        int got = 0;
        for (int i = 0; i < n; i++)
        {
            if (not kos::thread::create_caps(pool_probe_worker, nullptr, "probe", 10, caps, 1).valid())
            {
                break;
            }
            got++;
        }
        wait_n(got);
        return got == n;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    kos_cap_t g_pl_ep = KOS_CAP_NONE;
#endif
}
