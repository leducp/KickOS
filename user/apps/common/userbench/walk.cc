// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The owner-death walk, the word broadcast and the lost-mapping scan, each with up to every
// slot main's budget leaves seated waiting (docs/reference/invariants.md,
// mutex-owner-death-reaches-the-next-taker and sem-post-direct-handoff).

#include "walk.h"

#include "clock.h"

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/libc/fmt.h>

#ifndef USERBENCH_THREADS
#error "USERBENCH_THREADS is the bench's CMakeLists' to state"
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // main's own, from its composition.
    constexpr uint8_t MAIN_PRIO = 2;
    // Above main, so each runs to its park at main's next yield.
    constexpr uint8_t OD_PRIO = 3;
    // Above every walker, so nothing a walk readies runs inside its span.
    constexpr uint8_t TIMED_PRIO = 4;
    constexpr uint32_t OD_ROUNDS = 16;
    // Beside main; the owner walks also seat a holder.
    constexpr uint32_t OD_MAX = USERBENCH_THREADS - 1;
    static_assert(OD_MAX >= 2, "an owner walk needs a holder and a waiter beside main");

    kos_mutex_t g_od_m[OD_MAX];
    kos::thread::Handle g_od_waiter[OD_MAX];
    kos_sem_t g_od_die = KOS_SEM_INIT(0);
    // What the holder locks before it parks.
    kos_mutex_t* g_od_hold_first = nullptr;
    uint32_t g_od_hold_count = 0;
    uint32_t g_od_core_mask = 0;
    // The holder's last stamp before its exit.
    uint64_t g_od_exit_t0 = 0;
    // Every return no walk explains.
    Atomic<uint32_t, Order::RELAXED> g_od_odd{0};
    // Bumped by each walker before its park.
    Atomic<uint32_t, Order::RELAXED> g_od_parking{0};

    kos::thread::Handle od_spawn(void (*entry)(void*), void* arg, char const* name)
    {
        return kos::thread::create(entry, arg, name, OD_PRIO, KOS_POLICY_FIFO, 0, nullptr, 0,
                                   nullptr, 0, nullptr, 0, 0, nullptr, g_od_core_mask);
    }

    // The yield runs every seated walker to its park.
    void od_settle(uint32_t want)
    {
        kos_yield();
        if (g_od_parking != want)
        {
            g_od_odd = g_od_odd + 1u;
        }
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
        g_od_parking = g_od_parking + 1u;
        if (kos_sem_wait(&g_od_die, KOS_TIMEOUT_NONE) != 0)
        {
            g_od_odd = g_od_odd + 1u;
        }
        g_od_exit_t0 = bench::stamp();
    }

    void od_take(kos_mutex_t* m, int want)
    {
        g_od_parking = g_od_parking + 1u;
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

    // The holder parked, then up to `want` waiters parked until the kernel refuses one.
    // Returns those seated.
    uint32_t od_seat(void (*waiter)(void*), uint32_t want, kos::thread::Handle* holder)
    {
        g_od_parking = 0;
        *holder = od_spawn(od_holder, nullptr, "od_hold");
        if (not holder->valid())
        {
            return 0;
        }
        od_settle(1);
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
        od_settle(1u + seated);
        return seated;
    }

    void od_join_waiters(uint32_t seated)
    {
        for (uint32_t i = 0; i < seated; i++)
        {
            (void)g_od_waiter[i].join(KOS_TIMEOUT_NONE);
        }
    }

    // One record per waiter, each its own word: the walk's most records. The span runs from the
    // holder's last stamp to main's return from the join, main outranking every waiter it hands
    // a record to.
    uint32_t od_exit_round(uint32_t want, uint32_t* ns)
    {
        for (uint32_t i = 0; i < OD_MAX; i++)
        {
            g_od_m[i].word = 0;
        }
        g_od_hold_first = g_od_m;
        g_od_hold_count = want;
        kos::thread::Handle holder;
        uint32_t const seated = od_seat(od_waiter, want, &holder);
        if (not holder.valid())
        {
            return 0;
        }
        (void)kos_thread_set_priority(TIMED_PRIO);
        (void)kos_sem_post(&g_od_die);
        (void)holder.join(KOS_TIMEOUT_NONE);
        uint64_t const t1 = bench::stamp();
        *ns = bench::span_ns(g_od_exit_t0, t1);
        (void)kos_thread_set_priority(MAIN_PRIO);
        od_join_waiters(seated);
        return seated;
    }

    // Every waiter on one plain word, every one woken by one wake with no unit, as a condition
    // variable's broadcast wakes them.
    uint32_t g_ww_word = 0;

    void ww_waiter(void*)
    {
        g_od_parking = g_od_parking + 1u;
        if (kos_word_wait(&g_ww_word, KOS_WORD_WAITERS, KOS_TIMEOUT_NONE) != 0)
        {
            g_od_odd = g_od_odd + 1u;
        }
    }

    uint32_t ww_round(uint32_t want, uint32_t* ns)
    {
        g_ww_word = KOS_WORD_WAITERS;
        g_od_parking = 0;
        uint32_t seated = 0;
        while (seated < want)
        {
            kos::thread::Handle const w = od_spawn(ww_waiter, nullptr, "ww_wait");
            if (not w.valid())
            {
                break;
            }
            g_od_waiter[seated] = w;
            seated++;
        }
        od_settle(seated);
        (void)kos_thread_set_priority(TIMED_PRIO);
        uint64_t const t0 = bench::stamp();
        int const woken = kos_word_wake(&g_ww_word, 0xFFFFFFFFu, 0u);
        uint64_t const t1 = bench::stamp();
        *ns = bench::span_ns(t0, t1);
        (void)kos_thread_set_priority(MAIN_PRIO);
        if (woken != static_cast<int>(seated))
        {
            g_od_odd = g_od_odd + 1u;
        }
        od_join_waiters(seated);
        return seated;
    }

#if KICKOS_HAVE_ASPACE
    void od_end(kos::thread::Handle const& holder, uint32_t seated)
    {
        (void)kos_sem_post(&g_od_die);
        (void)holder.join(KOS_TIMEOUT_NONE);
        od_join_waiters(seated);
    }

    kos_cap_t g_od_frame = KOS_CAP_NONE;
    kos_cap_t g_od_space = KOS_CAP_NONE;

    void od_lost_waiter(void* arg)
    {
        od_take(static_cast<kos_mutex_t*>(arg), -KOS_EFAULT);
    }

    // Every waiter on ONE word, in a frame main unmaps under them: each wait the scan ends
    // recomputes the holder's priority over the waiters still left.
    uint32_t od_lost_round(uint32_t want, uint32_t* ns)
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
        (void)kos_thread_set_priority(TIMED_PRIO);
        uint64_t const t0 = bench::stamp();
        int const rc = kos_frame_unmap(g_od_frame, g_od_space, va);
        uint64_t const t1 = bench::stamp();
        *ns = bench::span_ns(t0, t1);
        (void)kos_thread_set_priority(MAIN_PRIO);
        if (rc != 0)
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

    struct Point
    {
        uint32_t k;
        uint32_t ns;
    };

    // OD_ROUNDS rounds at `want`; the point at the fewest any round seated.
    Point od_point(uint32_t (*round)(uint32_t, uint32_t*), uint32_t want, uint32_t* samples,
                   uint32_t tare)
    {
        bench::Series s = bench::series(samples, OD_ROUNDS);
        uint32_t fewest = want;
        for (uint32_t r = 0; r < OD_ROUNDS; r++)
        {
            uint32_t ns = 0;
            uint32_t const seated = round(want, &ns);
            if (seated < fewest)
            {
                fewest = seated;
            }
            s.add(ns);
        }
        Point p = {fewest, bench::less_tare(s.figure(), tare)};
        return p;
    }

    // No waiter, the most a round seats and the half: the slope is from the two ends.
    void od_row(char const* row, uint32_t (*round)(uint32_t, uint32_t*), uint32_t want,
                uint32_t* samples, uint32_t tare)
    {
        uint32_t probe = 0;
        uint32_t const most = round(want, &probe);
        char s[192];
        if (most < 1)
        {
            ksnprintf(s, sizeof(s), "  %s: SKIP (main's threads seat no waiter)\n", row);
            kos::print(s);
            return;
        }
        Point const hi = od_point(round, most, samples, tare);
        Point const mid = od_point(round, (hi.k + 1u) / 2u, samples, tare);
        Point const lo = od_point(round, 0, samples, tare);
        int32_t slope = 0;
        if (hi.k > lo.k)
        {
            slope = (static_cast<int32_t>(hi.ns) - static_cast<int32_t>(lo.ns))
                  / static_cast<int32_t>(hi.k - lo.k);
        }
        char const* fig = "mean";
        if (bench::FINE_CLOCK)
        {
            fig = "median";
        }
        ksnprintf(s, sizeof(s),
                  "  %s: %d ns/waiter  (k=%u %u ns, k=%u %u ns, k=%u %u ns; %s of %u)\n", row,
                  static_cast<int>(slope), static_cast<unsigned>(lo.k),
                  static_cast<unsigned>(lo.ns), static_cast<unsigned>(mid.k),
                  static_cast<unsigned>(mid.ns), static_cast<unsigned>(hi.k),
                  static_cast<unsigned>(hi.ns), fig, static_cast<unsigned>(OD_ROUNDS));
        kos::print(s);
    }
}

namespace bench
{
    void walks(uint32_t* samples, uint32_t tare)
    {
        int const core = kos_core_current();
        if (KICKOS_KERNEL_CORES > 1 and core >= 0 and core < 31)
        {
            g_od_core_mask = 1u << static_cast<uint32_t>(core);
            (void)kos_thread_affinity(kos_thread_self(), g_od_core_mask);
        }
        od_row("word-wake", ww_round, OD_MAX, samples, tare);
        od_row("owner-exit", od_exit_round, OD_MAX - 1u, samples, tare);
#if KICKOS_HAVE_ASPACE
        if (kos_frame_create(sizeof(kos_mutex_t), &g_od_frame) == 0
            and kos_aspace_self(&g_od_space) == 0)
        {
            od_row("reach-lost", od_lost_round, OD_MAX - 1u, samples, tare);
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
            ksnprintf(line, sizeof(line), "  walk: FAIL (%u odd returns)\n",
                      static_cast<unsigned>(static_cast<uint32_t>(g_od_odd)));
            kos::print(line);
        }
        if (KICKOS_KERNEL_CORES > 1)
        {
            (void)kos_thread_affinity(kos_thread_self(), 0);
        }
    }
}
