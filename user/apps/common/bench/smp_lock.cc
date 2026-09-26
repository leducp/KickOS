// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One runnable thread pinned to each kernel core. Each yield enters the shared
// scheduler lock, so the aggregate rate measures a genuinely parallel lock user.

#include <kickos/kos.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/emit.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

#ifndef KICKOS_SMP_LOCK_ROUNDS
#define KICKOS_SMP_LOCK_ROUNDS 2000000
#endif
    constexpr uint32_t ROUNDS = KICKOS_SMP_LOCK_ROUNDS;
    static_assert(ROUNDS > 0, "the lock benchmark needs at least one yield per core");
    constexpr uint64_t READY_LIMIT_NS = 5000000000ull;
    static_assert(KICKOS_MAX_THREADS >= KICKOS_KERNEL_CORES + 1,
                  "one root and one worker per kernel core must fit the thread pool");

    struct alignas(64) Worker
    {
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> ready{0};
        uint32_t core = 0;
        uint32_t completed = 0;
        uint64_t first_ns = 0;
        uint64_t last_ns = 0;
    };

    Worker g_worker[KICKOS_KERNEL_CORES] = {};
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_start{0};

    void run(void* arg)
    {
        Worker& w = *static_cast<Worker*>(arg);
        w.ready = 1;
        while (g_start == 0)
        {
            kos_sleep_ns(1000000ull);
        }
        w.first_ns = kos_clock_now();
        for (uint32_t i = 0; i < ROUNDS; ++i)
        {
            kos_yield();
            w.completed++;
        }
        w.last_ns = kos_clock_now();
    }

    void line(char const* fmt, unsigned a, unsigned b, unsigned c)
    {
        char out[128];
        ksnprintf(out, sizeof(out), fmt, a, b, c);
        kickos::emit(out);
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_SYSTEM);

int main(int, char**)
{
    kos_thread_t id[KICKOS_KERNEL_CORES] = {};
    for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; ++core)
    {
        Worker& w = g_worker[core];
        w.core = core;
        kos_thread_params p{};
        p.entry = run;
        p.arg = &w;
        p.name = "smp_yield";
        p.prio = 1;
        p.policy = KOS_POLICY_FIFO;
        p.core_mask = 1u << core;
        id[core] = KOS_THREAD_NONE;
        int const rc = kos_thread_create(&p, &id[core]);
        if (rc != 0)
        {
            line("smp-lock: spawn failed core=%u rc=%u created=%u\n",
                 static_cast<unsigned>(core), static_cast<unsigned>(-rc),
                 static_cast<unsigned>(core));
            return 1;
        }
    }

    uint64_t const deadline = kos_clock_now() + READY_LIMIT_NS;
    while (true)
    {
        uint32_t ready = 0;
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; ++core)
        {
            ready += g_worker[core].ready.load();
        }
        if (ready == KICKOS_KERNEL_CORES)
        {
            break;
        }
        if (kos_clock_now() > deadline)
        {
            line("smp-lock: ready timeout ready=%u need=%u\n",
                 static_cast<unsigned>(ready), static_cast<unsigned>(KICKOS_KERNEL_CORES), 0);
            return 1;
        }
        kos_sleep_ns(1000000ull);
    }

    uint64_t const start_ns = kos_clock_now();
    g_start = 1;
    for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; ++core)
    {
        int const rc = kos_thread_join(id[core], KOS_TIMEOUT_NONE);
        if (rc != 0)
        {
            line("smp-lock: join failed core=%u rc=%u\n",
                 static_cast<unsigned>(core), static_cast<unsigned>(-rc), 0);
            return 1;
        }
    }
    uint64_t const elapsed = kos_clock_now() - start_ns;

    uint32_t completed = 0;
    uint64_t earliest = UINT64_MAX;
    uint64_t latest = 0;
    for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; ++core)
    {
        Worker const& w = g_worker[core];
        completed += w.completed;
        if (w.first_ns < earliest) { earliest = w.first_ns; }
        if (w.last_ns > latest) { latest = w.last_ns; }
        line("smp-lock: core=%u yields=%u active_ms=%u\n",
             static_cast<unsigned>(core), static_cast<unsigned>(w.completed),
             static_cast<unsigned>((w.last_ns - w.first_ns) / 1000000ull));
    }
    uint32_t rate = 0;
    if (elapsed != 0)
    {
        rate = static_cast<uint32_t>(static_cast<uint64_t>(completed) * 1000000000ull / elapsed);
    }
    char out[160];
    ksnprintf(out, sizeof(out),
              "smp-lock: cores=%u yields=%u wall_ms=%u span_ms=%u rate=%u/s\n",
              static_cast<unsigned>(KICKOS_KERNEL_CORES), static_cast<unsigned>(completed),
              static_cast<unsigned>(elapsed / 1000000ull),
              static_cast<unsigned>((latest - earliest) / 1000000ull),
              static_cast<unsigned>(rate));
    kickos::emit(out);
    if (completed != ROUNDS * KICKOS_KERNEL_CORES)
    {
        return 1;
    }
    kickos::emit("smp-lock: done\n");
    return 0;
}
