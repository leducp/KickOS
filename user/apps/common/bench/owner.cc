// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The owner-death walk and the lost-mapping scan, each with every slot the task can still seat
// waiting (docs/reference/invariants.md, mutex-owner-death-reaches-the-next-taker).

#include "owner.h"

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/libc/fmt.h>

#ifndef BENCH_OD_WAITERS_MAX
#error "BENCH_OD_WAITERS_MAX is the bench's CMakeLists' to state"
#endif

#if BENCH_OD_WAITERS_MAX > 0
namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Above main's priority and pinned to main's core: a spawned thread runs to its park before
    // the spawn returns, and a woken one ends before main runs again.
    constexpr uint8_t OD_PRIO = 3;
    constexpr uint32_t OD_ROUNDS = 16;
    constexpr uint32_t OD_MAX = BENCH_OD_WAITERS_MAX;

    kos_mutex_t g_od_m[OD_MAX];
    kos::thread::Handle g_od_waiter[OD_MAX];
    kos_sem_t g_od_die = KOS_SEM_INIT(0);
    // What the holder locks before it parks.
    kos_mutex_t* g_od_hold_first = nullptr;
    uint32_t g_od_hold_count = 0;
    uint32_t g_od_core_mask = 0;
    // Every return no walk explains.
    Atomic<uint32_t, Order::RELAXED> g_od_odd{0};

    kos::thread::Handle od_spawn(void (*entry)(void*), void* arg, char const* name)
    {
        return kos::thread::create(entry, arg, name, OD_PRIO, KOS_POLICY_FIFO, 0, nullptr, 0,
                                   nullptr, 0, nullptr, 0, 0, nullptr, g_od_core_mask);
    }

    // Returns holding every word it took: its exit is the walk.
    void od_holder(void*)
    {
        for (uint32_t i = 0; i < g_od_hold_count; i++)
        {
            if (kos_mutex_lock(&g_od_hold_first[i], 0) != 0)
            {
                g_od_odd = g_od_odd + 1u;
            }
        }
        if (kos_sem_wait(&g_od_die, KOS_TIMEOUT_NONE) != 0)
        {
            g_od_odd = g_od_odd + 1u;
        }
    }

    void od_take(kos_mutex_t* m, int want)
    {
        int const rc = kos_mutex_lock(m, KOS_TIMEOUT_NONE);
        if (rc != want)
        {
            g_od_odd = g_od_odd + 1u;
        }
        if (rc == 0 or rc == -KOS_EOWNERDEAD)
        {
            (void)kos_mutex_unlock(m);
        }
    }

    void od_waiter(void* arg)
    {
        od_take(static_cast<kos_mutex_t*>(arg), -KOS_EOWNERDEAD);
    }

    // The holder, then up to `want` waiters until the kernel refuses one. Returns those seated.
    uint32_t od_seat(void (*waiter)(void*), uint32_t want, kos::thread::Handle* holder)
    {
        *holder = od_spawn(od_holder, nullptr, "od_hold");
        if (not holder->valid())
        {
            return 0;
        }
        uint32_t seated = 0;
        while (seated < want)
        {
            void* arg = g_od_hold_first;
            if (g_od_hold_count > 1u)
            {
                arg = &g_od_hold_first[seated];
            }
            kos::thread::Handle const w = od_spawn(waiter, arg, "od_wait");
            if (not w.valid())
            {
                break;
            }
            g_od_waiter[seated] = w;
            seated++;
        }
        return seated;
    }

    void od_end(kos::thread::Handle const& holder, uint32_t seated)
    {
        (void)kos_sem_post(&g_od_die);
        (void)holder.join(KOS_TIMEOUT_NONE);
        for (uint32_t i = 0; i < seated; i++)
        {
            (void)g_od_waiter[i].join(KOS_TIMEOUT_NONE);
        }
    }

    // One record per waiter, each its own word: the walk's most records.
    uint32_t od_exit_round(uint32_t want)
    {
        for (uint32_t i = 0; i < OD_MAX; i++)
        {
            g_od_m[i].word = 0;
        }
        g_od_hold_first = g_od_m;
        g_od_hold_count = want;
        kos::thread::Handle holder;
        uint32_t const seated = od_seat(od_waiter, want, &holder);
        if (holder.valid())
        {
            od_end(holder, seated);
        }
        return seated;
    }

    // Runs `rounds` rounds of `round` at `want`; returns the most it seated.
    uint32_t od_step(uint32_t (*round)(uint32_t), uint32_t want)
    {
        uint32_t most = 0;
        for (uint32_t r = 0; r < OD_ROUNDS; r++)
        {
            uint32_t const seated = round(want);
            if (seated > most)
            {
                most = seated;
            }
        }
        return most;
    }

    // The most every round seats, the half and one: the rows grow with the records or they do
    // not.
    void od_steps(uint32_t which, uint32_t (*round)(uint32_t))
    {
        uint32_t const most = round(OD_MAX);
        uint32_t seated = od_step(round, most);
        if (most > seated)
        {
            seated = most;
        }
        (void)kos_bench(KOS_BENCH_OP_OWNER_PRINT, which, seated);
        uint32_t half = seated / 2u;
        if (half == 0u)
        {
            half = 1u;
        }
        (void)kos_bench(KOS_BENCH_OP_OWNER_PRINT, which, od_step(round, half));
        (void)kos_bench(KOS_BENCH_OP_OWNER_PRINT, which, od_step(round, 1u));
    }

#if defined(KICKOS_HAVE_ASPACE) && KICKOS_HAVE_ASPACE
    kos_cap_t g_od_frame = KOS_CAP_NONE;
    kos_cap_t g_od_space = KOS_CAP_NONE;

    void od_lost_waiter(void* arg)
    {
        od_take(static_cast<kos_mutex_t*>(arg), -KOS_EFAULT);
    }

    // Every waiter on ONE word, in a frame main unmaps under them: each wait the scan ends
    // recomputes the holder's priority over the waiters still left.
    uint32_t od_lost_round(uint32_t want)
    {
        uintptr_t va = 0;
        if (kos_frame_map(g_od_frame, g_od_space, &va, 0) != 0 or va == 0)
        {
            g_od_odd = g_od_odd + 1u;
            return 0;
        }
        kos_mutex_t* const word = reinterpret_cast<kos_mutex_t*>(va);
        word->word = 0;
        g_od_hold_first = word;
        g_od_hold_count = 1;
        kos::thread::Handle holder;
        uint32_t const seated = od_seat(od_lost_waiter, want, &holder);
        if (kos_frame_unmap(g_od_frame, g_od_space, va) != 0)
        {
            g_od_odd = g_od_odd + 1u;
        }
        if (holder.valid())
        {
            od_end(holder, seated);
        }
        return seated;
    }
#endif
}

#endif

namespace bench
{
    void owner_death()
    {
#if BENCH_OD_WAITERS_MAX > 0
        int const core = kos_core_current();
        if (core >= 0 and core < 31)
        {
            g_od_core_mask = 1u << static_cast<uint32_t>(core);
            (void)kos_thread_affinity(kos_thread_self(), g_od_core_mask);
        }
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        od_steps(KOS_BENCH_OWNER_EXIT, od_exit_round);
#if defined(KICKOS_HAVE_ASPACE) && KICKOS_HAVE_ASPACE
        if (kos_frame_create(sizeof(kos_mutex_t), &g_od_frame) == 0
            and kos_aspace_self(&g_od_space) == 0)
        {
            od_steps(KOS_BENCH_OWNER_REACH_LOST, od_lost_round);
        }
        else
        {
            g_od_odd = g_od_odd + 1u;
        }
        if (g_od_frame != KOS_CAP_NONE)
        {
            (void)kos_handle_close(g_od_frame);
        }
        if (g_od_space != KOS_CAP_NONE)
        {
            (void)kos_handle_close(g_od_space);
        }
#endif
        if (g_od_odd != 0u)
        {
            char line[64];
            ksnprintf(line, sizeof(line), "  owner-death: %u odd return(s)\n",
                      static_cast<unsigned>(static_cast<uint32_t>(g_od_odd)));
            kos::print(line);
        }
        (void)kos_thread_affinity(kos_thread_self(), 0);
#endif
    }
}
