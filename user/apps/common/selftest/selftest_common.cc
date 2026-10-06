// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The definitions every self-test translation unit reaches.

#include "selftest.h"

namespace selftest
{
    kos_cap_t g_done = KOS_CAP_NONE; // shared completion counter (MAIN's cap; delegated to workers)
    kos_cap_t g_lock = KOS_CAP_NONE; // binary semaphore = mutex over the event log (MAIN's cap)
    kos_self_t const* g_self = nullptr;
    kos_thread_t g_main = KOS_THREAD_NONE;

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

    void pool_probe_worker(void*) // caps: done@1
    {
        kos_sem_post(CH_DONE);
    }

    // Above every batch an arm asks for: a crowd of one per kernel core plus one, or four.
    constexpr int POOL_PROBES_MAX = KICKOS_KERNEL_CORES + 4;

    void pool_gated_worker(void*) // caps: gate@1
    {
        kos_sem_wait(1);
    }

    // Can this board host `n` workers CONCURRENTLY, right now? Slots held by driver tasks
    // and room for each stack bound this as much as the thread pool's size does.
    // No probe leaves before the whole batch exists, and each is joined, so on return every
    // probe slot and stack is free again. Call immediately before the real spawns.
    bool pool_can_host(int n)
    {
        if (n > POOL_PROBES_MAX)
        {
            tap::fail("pool_can_host(%d) is wider than its %d probe handles", n, POOL_PROBES_MAX);
            return false;
        }
        kos_cap_t gate = KOS_CAP_NONE;
        if (kos_sem_create(0, &gate) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{gate, KOS_CAP_WAIT}};
        kos::thread::Handle probes[POOL_PROBES_MAX];
        int got = 0;
        for (int i = 0; i < n; i++)
        {
            probes[i] = kos::thread::create_caps(pool_gated_worker, nullptr, "probe", 10, caps, 1);
            if (not probes[i].valid())
            {
                break;
            }
            got++;
        }
        for (int i = 0; i < got; i++)
        {
            kos_sem_post(gate);
        }
        for (int i = 0; i < got; i++)
        {
            (void)probes[i].join();
        }
        (void)kos_handle_close(gate);
        return got == n;
    }

    namespace
    {
        enum class ObjectKind
        {
            SEM,
            MUTEX,
            ENDPOINT,
            NOTIFY,
        };

        // Direct calls only: the trap red-zone gate refuses an indirect edge it cannot name.
        int make(ObjectKind kind, kos_cap_t* out)
        {
            switch (kind)
            {
            case ObjectKind::SEM:
            {
                return kos_sem_create(0, out);
            }
            case ObjectKind::MUTEX:
            {
                return kos_mutex_create(out);
            }
            case ObjectKind::ENDPOINT:
            {
                return kos_endpoint_create(out);
            }
            case ObjectKind::NOTIFY:
            {
                return kos_notify_create(out);
            }
            }
            return -KOS_EINVAL;
        }

        // Creates `count` objects of `kind` into held[*n], answering the first refusal.
        int hold(int count, ObjectKind kind, kos_cap_t* held, int* n)
        {
            for (int i = 0; i < count; i++)
            {
                kos_cap_t h = KOS_CAP_NONE;
                int const rc = make(kind, &h);
                if (rc != 0)
                {
                    return rc;
                }
                held[*n] = h;
                *n = *n + 1;
            }
            return 0;
        }
    }

    // Can main's task hold all of `want` at once, right now? 0 if it can, else the first
    // refusal, and every object is closed again before it returns. A refusal that is not a
    // supply running out fails the arm asking. Call immediately before the arm's own creates.
    int objects_can_host(ObjectDemand want)
    {
        constexpr int MAX = 16;
        int const total = want.sems + want.mutexes + want.endpoints + want.notifies;
        if (total > MAX)
        {
            tap::fail("objects_can_host(%d objects) is wider than its %d handles", total, MAX);
            return -KOS_EINVAL;
        }
        kos_cap_t held[MAX];
        int n = 0;
        int rc = hold(want.sems, ObjectKind::SEM, held, &n);
        if (rc == 0)
        {
            rc = hold(want.mutexes, ObjectKind::MUTEX, held, &n);
        }
        if (rc == 0)
        {
            rc = hold(want.endpoints, ObjectKind::ENDPOINT, held, &n);
        }
        if (rc == 0)
        {
            rc = hold(want.notifies, ObjectKind::NOTIFY, held, &n);
        }
        for (int i = 0; i < n; i++)
        {
            (void)kos_handle_close(held[i]);
        }
        if (rc != 0 and rc != -KOS_EMFILE and rc != -KOS_EAGAIN and rc != -KOS_ENOMEM)
        {
            tap::fail("objects_can_host: a create answered %d", rc);
        }
        return rc;
    }

    bool g_ram_starved = false;

    void* st_ram_alloc_as(bool starved, size_t size)
    {
        if (starved)
        {
            return nullptr;
        }
        return kos_ram_alloc(size);
    }

    void* st_ram_alloc(size_t size)
    {
        return st_ram_alloc_as(g_ram_starved, size);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    kos_cap_t g_pl_ep = KOS_CAP_NONE;
#endif
}
