// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// std::atomic read-modify-writes in user code, raced. Round-robin threads of one priority, sliced
// at one clock granule, each fetch_add one std::atomic<int> and one std::atomic<uint8_t>, and
// fetch_sub the std::atomic<uint16_t> that shares the byte's word, for a run's length and until
// another's add has landed between two of its own. The run passes when the three totals are the
// exact ones the workers' rounds make and every worker saw such an add.

#include <kickos/kos.h>

#include <kickos/libc/fmt.h>

#include <atomic>
#include <stdint.h>

namespace
{
    constexpr unsigned WORKERS = 3;
    constexpr uint64_t RUN_NS = 100000000ull;
    // A worker goes on past the run's length until another's add has landed, up to this many.
    constexpr uint64_t RUN_LIMIT = 8u;
    // Below main, so no worker runs before all are spawned.
    constexpr uint8_t WORKER_PRIO = 1;

    struct alignas(4) Lanes
    {
        std::atomic<uint8_t> byte;
        std::atomic<uint16_t> half;
    };
    static_assert(sizeof(Lanes) == 4, "the byte and the half share one word");

    std::atomic<int> g_count{0};
    Lanes g_lanes{};
    volatile uint64_t g_run_ns = 0;
    volatile uint32_t g_rounds[WORKERS] = {};
    volatile uint32_t g_interleaved[WORKERS] = {};

    uint64_t clock_granule_ns()
    {
        uint64_t const e0 = kos_clock_now();
        uint64_t e1 = e0;
        while (e1 == e0)
        {
            e1 = kos_clock_now();
        }
        uint64_t e2 = e1;
        while (e2 == e1)
        {
            e2 = kos_clock_now();
        }
        return e2 - e1;
    }

    void worker(void* arg)
    {
        uintptr_t const me = reinterpret_cast<uintptr_t>(arg);
        uint64_t const start = kos_clock_now();
        uint32_t interleaved = 0;
        uint32_t rounds = 0;
        int last = 0;
        while (true)
        {
            int const was = g_count.fetch_add(1);
            if (rounds != 0u and was != last + 1)
            {
                interleaved = interleaved + 1u;
            }
            last = was;
            g_lanes.byte.fetch_add(1u);
            g_lanes.half.fetch_sub(1u);
            rounds = rounds + 1u;
            // The clock rarely: a read is a kernel entry, and the switches must land in the loops.
            if ((rounds & 1023u) == 0u)
            {
                uint64_t const ran = kos_clock_now() - start;
                if (ran >= g_run_ns * RUN_LIMIT)
                {
                    break;
                }
                if (ran >= g_run_ns and interleaved != 0u)
                {
                    break;
                }
            }
        }
        g_rounds[me] = rounds;
        g_interleaved[me] = interleaved;
    }

    bool exact(char const* name, uint32_t got, uint32_t want)
    {
        char msg[96];
        if (got != want)
        {
            ksnprintf(msg, sizeof(msg), "uatomic: FAIL %s %u, want %u\n", name,
                      static_cast<unsigned>(got), static_cast<unsigned>(want));
            kos_print(msg);
            return false;
        }
        ksnprintf(msg, sizeof(msg), "uatomic: ok %s %u\n", name, static_cast<unsigned>(got));
        kos_print(msg);
        return true;
    }
}

int main(int, char**)
{
    uint64_t const quantum = clock_granule_ns();
    uint64_t run = RUN_NS;
    if (run < quantum * 64u)
    {
        run = quantum * 64u;
    }
    g_run_ns = run;
    char msg[96];
    ksnprintf(msg, sizeof(msg), "uatomic: %u workers, quantum %u ns\n", WORKERS,
              static_cast<unsigned>(quantum));
    kos_print(msg);
    kos::thread::Handle workers[WORKERS];
    for (unsigned w = 0; w < WORKERS; w = w + 1u)
    {
        workers[w] = kos::thread::create(worker, reinterpret_cast<void*>(uintptr_t{w}), "uat",
                                         WORKER_PRIO, KOS_POLICY_RR,
                                         static_cast<uint32_t>(quantum));
        if (not workers[w].valid())
        {
            kos_print("uatomic: FAIL a worker did not spawn\n");
            return 1;
        }
    }
    for (unsigned w = 0; w < WORKERS; w = w + 1u)
    {
        (void)workers[w].join(KOS_TIMEOUT_NONE);
    }
    bool ok = true;
    uint32_t total = 0;
    for (unsigned w = 0; w < WORKERS; w = w + 1u)
    {
        ksnprintf(msg, sizeof(msg), "uatomic: worker %u ran %u rounds, interleaved %u times\n", w,
                  static_cast<unsigned>(g_rounds[w]), static_cast<unsigned>(g_interleaved[w]));
        kos_print(msg);
        total = total + g_rounds[w];
        if (g_interleaved[w] == 0u)
        {
            kos_print("uatomic: FAIL a worker saw no other worker's add between two of its own\n");
            ok = false;
        }
    }
    ok = exact("int", static_cast<uint32_t>(g_count.load()), total) and ok;
    ok = exact("byte", g_lanes.byte.load(), static_cast<uint8_t>(total)) and ok;
    ok = exact("half", g_lanes.half.load(), static_cast<uint16_t>(0u - total)) and ok;
    if (not ok)
    {
        return 1;
    }
    kos_print("uatomic: done\n");
    return 0;
}
