// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_MUTEXBENCH_HANDOFF_H
#define KICKOS_USER_APPS_COMMON_MUTEXBENCH_HANDOFF_H

#include <stdint.h>

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>

namespace mutexbench
{
    using kickos::Atomic;
    using kickos::Order;

    constexpr uint32_t HANDOFFS = 1000;

    // What a holder and a waiter share over one handoff run.
    struct Handoff
    {
        kos_mutex_t m;
        kos_sem_t go;
        kos_sem_t done;
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> enter;
        Atomic<uint32_t, Order::RELAXED> taken;
        uint32_t rounds;
        // Written by the holder before its unlock, read by the waiter after its lock returns.
        uint64_t t0;
    };

    inline void waiter_run(Handoff* ho, uint32_t* wait)
    {
        uint32_t r = 0;
        while (true)
        {
            kos_sem_wait(&ho->go, KOS_TIMEOUT_NONE);
            if (r == ho->rounds)
            {
                return;
            }
            r++;
            ho->enter = r;
            if (kos_mutex_lock(&ho->m, KOS_TIMEOUT_NONE) != 0)
            {
                return;
            }
            uint64_t d = kos::clock_now() - ho->t0;
            if (d > UINT32_MAX)
            {
                d = UINT32_MAX;
            }
            if (wait != nullptr)
            {
                wait[r - 1u] = static_cast<uint32_t>(d);
            }
            ho->taken = r;
            (void)kos_mutex_unlock(&ho->m);
            kos_sem_post(&ho->done);
        }
    }

    // The window leg's handoff and samples, in a block both tasks reach.
    struct WindowBlock
    {
        Handoff ho;
        uint32_t wait[HANDOFFS];
    };
}

#endif
