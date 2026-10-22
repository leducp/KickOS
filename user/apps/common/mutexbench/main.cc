// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex as an application pays for it: an uncontended lock and unlock, the C library's locks
// under malloc and getenv, the bare syscalls, a semaphore's post and wait, and the waiter's wait in a
// handoff, alone, beside other parked pairs, and from another task through a memory window, a
// semaphore round trip between two threads, and a post with every spare thread parked on the
// semaphore. Timed on kos_clock_now, so
// it runs on every board and needs no bench kernel; a bench kernel also prints its own rows over
// each handoff window.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/driver_service.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <stdlib.h>

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    constexpr uint32_t REPS = 20000;
    // Under a coarse clock, so a row spans many of its steps.
    constexpr uint32_t COARSE_REPS = 200000;
    constexpr uint32_t COARSE_ROUNDS = 20000;
    constexpr uint32_t HANDOFFS = 1000;
    constexpr uint32_t MALLOC_BYTES = 32;
    // A clock stepping coarser than this cannot time one handoff, so only the round row prints.
    constexpr uint64_t COARSE_CLOCK_NS = 100000;
    // Above one kernel core the holder cannot see the waiter park: it waits this long after the
    // announcement, and a waiter its host stalls past that takes the mutex free.
    constexpr uint64_t SETTLE_NS = 50000;
    constexpr uint64_t QUIET_NS = 500000000;
    constexpr uint8_t HOLDER_PRIO = 4;
    // Above the holder, so at one kernel core the holder runs again only once the waiter parks.
    constexpr uint8_t WAITER_PRIO = 5;
    // Below the handoff pair, so a parked load thread never runs inside a timed round.
    constexpr uint8_t LOAD_PRIO = 3;
    // The time a load waiter has, once announced, to park on its holder's mutex.
    constexpr uint64_t LOAD_SETTLE_NS = 20000000;
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t HOLDER_CORES = 1u << 0;
    constexpr uint32_t WAITER_CORES = 1u << 1;
#else
    constexpr uint32_t HOLDER_CORES = 0;
    constexpr uint32_t WAITER_CORES = 0;
#endif

    // Every thread slot but root's, the idle thread's and the handoff pair's, two per pair.
    constexpr uint32_t load_pairs()
    {
        if (KICKOS_MAX_THREADS + 1 < 4)
        {
            return 0;
        }
        return (KICKOS_MAX_THREADS + 1 - 4) / 2;
    }
    constexpr uint32_t LOAD_PAIRS = load_pairs();

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

    kos::Mutex g_bench_m;
    Handoff g_ho = {KOS_MUTEX_INIT, KOS_SEM_INIT(0), KOS_SEM_INIT(0), 0, 0, HANDOFFS, 0};
    // A region board denies a thread its spawner's stack.
    Handoff g_sho = {KOS_MUTEX_INIT, KOS_SEM_INIT(0), KOS_SEM_INIT(0), 0, 0, HANDOFFS, 0};
    // One word per load pair; one more so the array is never empty.
    kos_mutex_t g_load_m[LOAD_PAIRS + 1] = {};
    // Every load pair's: the holders park on release, and each thread posts ready once it stands.
    kos_sem_t g_release = KOS_SEM_INIT(0);
    kos_sem_t g_ready = KOS_SEM_INIT(0);
    kos_sem_t g_own = KOS_SEM_INIT(0);

    void* volatile g_sink = nullptr;
    char const* volatile g_env = nullptr;
    // From the heap, and only where the clock can time a handoff: a small board has no room.
    uint32_t* g_wait = nullptr;

    void line(char const* what, char const* unit, uint32_t reps, uint64_t d_ns)
    {
        char s[160];
        ksnprintf(s, sizeof(s), "  %s %u ns/%s  (%u %ss / %u ms)\n", what,
                  static_cast<unsigned>(d_ns / reps), unit, static_cast<unsigned>(reps), unit,
                  static_cast<unsigned>(d_ns / 1000000ull));
        kos::print(s);
    }

    uint64_t clock_step()
    {
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < 8; i++)
        {
            uint64_t const a = kos::clock_now();
            uint64_t b = kos::clock_now();
            while (b == a)
            {
                b = kos::clock_now();
            }
            if (b - a < best)
            {
                best = b - a;
            }
        }
        return best;
    }

    enum Row
    {
        ROW_MUTEX,
        ROW_MALLOC,
        ROW_GETENV,
        ROW_SELF,
        ROW_BARE,
        ROW_CLOCK,
        ROW_CORE,
        ROW_SEM,
        ROW_COUNT
    };
    // Each row's run, printed only once every row has run: a line draining on the console would
    // land in the next row's time.
    uint64_t g_row_ns[ROW_COUNT] = {};

    void uncontended(uint32_t reps)
    {
        (void)g_bench_m.lock();
        (void)g_bench_m.unlock();
        uint64_t t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            (void)g_bench_m.lock();
            (void)g_bench_m.unlock();
        }
        g_row_ns[ROW_MUTEX] = kos::clock_now() - t0;

        g_sink = malloc(MALLOC_BYTES);
        free(g_sink);
        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            g_sink = malloc(MALLOC_BYTES);
            free(g_sink);
        }
        g_row_ns[ROW_MALLOC] = kos::clock_now() - t0;

        // The environment lock is the libc lock, and a name nobody set allocates nothing.
        g_env = getenv("KICKOS_MUTEXBENCH_UNSET");
        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            g_env = getenv("KICKOS_MUTEXBENCH_UNSET");
        }
        g_row_ns[ROW_GETENV] = kos::clock_now() - t0;

        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            (void)kos_thread_self();
        }
        g_row_ns[ROW_SELF] = kos::clock_now() - t0;

        // A kernel entry's floor: a syscall on every board, under no kernel lock; it must stay one.
        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            (void)kos_cpu_clock_hz();
        }
        g_row_ns[ROW_BARE] = kos::clock_now() - t0;

        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            (void)kos_clock_now();
        }
        g_row_ns[ROW_CLOCK] = kos::clock_now() - t0;

        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            (void)kos_core_current();
        }
        g_row_ns[ROW_CORE] = kos::clock_now() - t0;

        kos_sem_post(&g_own);
        kos_sem_wait(&g_own, KOS_TIMEOUT_NONE);
        t0 = kos::clock_now();
        for (uint32_t i = 0; i < reps; i++)
        {
            kos_sem_post(&g_own);
            kos_sem_wait(&g_own, KOS_TIMEOUT_NONE);
        }
        g_row_ns[ROW_SEM] = kos::clock_now() - t0;
    }

    void uncontended_print(uint32_t reps)
    {
        line("mutex: lock+unlock", "pair", reps, g_row_ns[ROW_MUTEX]);
#if KICKOS_ARCH_SIM
        kos::print("  libc-lock: SKIP (the sim's C library is the host's)\n");
#else
        line("libc-lock: malloc+free", "pair", reps, g_row_ns[ROW_MALLOC]);
        line("libc-lock: getenv", "call", reps, g_row_ns[ROW_GETENV]);
#endif
        line("syscall: thread_self", "call", reps, g_row_ns[ROW_SELF]);
        line("syscall: cpu_clock_hz", "call", reps, g_row_ns[ROW_BARE]);
        line("syscall: clock_now", "call", reps, g_row_ns[ROW_CLOCK]);
        line("syscall: core_current", "call", reps, g_row_ns[ROW_CORE]);
        line("sem: post+wait", "pair", reps, g_row_ns[ROW_SEM]);
    }

    void holder(void* arg)
    {
        Handoff* const ho = static_cast<Handoff*>(arg);
        for (uint32_t r = 1; r <= ho->rounds; r++)
        {
            if (kos_mutex_lock(&ho->m, KOS_TIMEOUT_NONE) != 0)
            {
                break;
            }
            kos_sem_post(&ho->go);
            while (ho->enter != r)
            {
#if KICKOS_KERNEL_CORES == 1
                kos_yield();
#endif
            }
#if KICKOS_KERNEL_CORES > 1
            uint64_t const seen = kos::clock_now();
            while (kos::clock_now() - seen < SETTLE_NS)
            {
            }
#endif
            ho->t0 = kos::clock_now();
            (void)kos_mutex_unlock(&ho->m);
            kos_sem_wait(&ho->done, KOS_TIMEOUT_NONE);
        }
        kos_sem_post(&ho->go);
    }

    void waiter_run(Handoff* ho, uint32_t* wait)
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

    void waiter(void* arg)
    {
        waiter_run(static_cast<Handoff*>(arg), g_wait);
    }

    // A round trip's time, from the pinger's post to its wait returning.
    void pinger(void* arg)
    {
        Handoff* const ho = static_cast<Handoff*>(arg);
        for (uint32_t r = 1; r <= ho->rounds; r++)
        {
            uint64_t const t0 = kos::clock_now();
            kos_sem_post(&ho->go);
            if (kos_sem_wait(&ho->done, KOS_TIMEOUT_NONE) != 0)
            {
                return;
            }
            uint64_t d = kos::clock_now() - t0;
            if (d > UINT32_MAX)
            {
                d = UINT32_MAX;
            }
            if (g_wait != nullptr)
            {
                g_wait[r - 1u] = static_cast<uint32_t>(d);
            }
            ho->taken = r;
        }
    }

    void ponger(void* arg)
    {
        Handoff* const ho = static_cast<Handoff*>(arg);
        for (uint32_t r = 1; r <= ho->rounds; r++)
        {
            if (kos_sem_wait(&ho->go, KOS_TIMEOUT_NONE) != 0)
            {
                return;
            }
            kos_sem_post(&ho->done);
        }
    }

#if KICKOS_HAVE_ASPACE
    // The window leg's handoff and samples, in a block both tasks reach.
    struct WindowBlock
    {
        Handoff ho;
        uint32_t wait[HANDOFFS];
    };

    // In a child task, whose statics are its own copies: all it shares is in its window.
    void window_waiter(void*)
    {
        kos_window at = {};
        if (kos_window_get(0, &at) != 0)
        {
            return;
        }
        WindowBlock* const b = reinterpret_cast<WindowBlock*>(at.base);
        waiter_run(&b->ho, b->wait);
    }
#endif

    void load_holder(void* arg)
    {
        kos_mutex_t* const m = static_cast<kos_mutex_t*>(arg);
        if (kos_mutex_lock(m, KOS_TIMEOUT_NONE) != 0)
        {
            kos_sem_post(&g_ready);
            return;
        }
        kos_sem_post(&g_ready);
        kos_sem_wait(&g_release, KOS_TIMEOUT_NONE);
        (void)kos_mutex_unlock(m);
    }

    void load_waiter(void* arg)
    {
        kos_mutex_t* const m = static_cast<kos_mutex_t*>(arg);
        kos_sem_post(&g_ready);
        if (kos_mutex_lock(m, KOS_TIMEOUT_NONE) == 0)
        {
            (void)kos_mutex_unlock(m);
        }
    }

    kos::thread::Handle g_load_t[2 * LOAD_PAIRS + 1];
    uint32_t g_load_threads = 0;
    uint32_t g_load_holders = 0;
    // What refused the next pair, when fewer than LOAD_PAIRS stand.
    char const* g_load_stop = "";
    int g_load_err = 0;

    // Parks up to LOAD_PAIRS holders on g_release, each with one waiter parked on its mutex, and
    // answers how many pairs stand.
    uint32_t load_up()
    {
        uint32_t pairs = 0;
        while (pairs < LOAD_PAIRS)
        {
            kos_mutex_t* const lm = &g_load_m[pairs];
            auto h = kos::thread::create(load_holder, lm, "mx_lh", LOAD_PRIO);
            if (not h.valid())
            {
                g_load_stop = "thread";
                g_load_err = h.error();
                break;
            }
            g_load_t[g_load_threads] = h;
            g_load_threads++;
            g_load_holders++;
            kos_sem_wait(&g_ready, KOS_TIMEOUT_NONE);
            auto w = kos::thread::create(load_waiter, lm, "mx_lw", LOAD_PRIO);
            if (not w.valid())
            {
                g_load_stop = "thread";
                g_load_err = w.error();
                break;
            }
            g_load_t[g_load_threads] = w;
            g_load_threads++;
            kos_sem_wait(&g_ready, KOS_TIMEOUT_NONE);
            pairs++;
        }
        kos::sleep_ns(LOAD_SETTLE_NS);
        return pairs;
    }

    void load_down()
    {
        for (uint32_t i = 0; i < g_load_holders; i++)
        {
            kos_sem_post(&g_release);
        }
        for (uint32_t i = 0; i < g_load_threads; i++)
        {
            (void)g_load_t[i].join(KOS_TIMEOUT_NONE);
        }
        g_load_threads = 0;
        g_load_holders = 0;
    }

    // The kernel rows' rank rule, over exact samples rather than bucket floors.
    uint32_t ranked(uint32_t const* wait, uint32_t n, uint32_t num, uint32_t den)
    {
        uint32_t const k = static_cast<uint32_t>((static_cast<uint64_t>(n) * num + den - 1u) / den);
        return wait[k - 1u];
    }

    // Runs ho->rounds handoffs of ho->m between a fresh holder and waiter, the waiter's wait from
    // the holder's unlock to its own lock returning, both clock reads included. The waiter runs
    // `entry` in `task` with `win`. False when either thread could not spawn.
    bool handoff_run(Handoff* ho, void (*entry)(void*), kos_task_t task, kos_window const* win,
                     uint16_t win_count, uint64_t* d_ns)
    {
        ho->enter = 0;
        ho->taken = 0;
        (void)kos_sem_init(&ho->go, 0);
        (void)kos_sem_init(&ho->done, 0);
#if KICKOS_BENCH
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
#endif
        uint64_t const t0 = kos::clock_now();
        auto w = kos::thread::create(entry, ho, "mx_w", WAITER_PRIO, KOS_POLICY_FIFO, 0, nullptr,
                                     0, nullptr, 0, win, win_count, nullptr, 0, 0, nullptr, task,
                                     WAITER_CORES);
        auto h = kos::thread::create_caps(holder, ho, "mx_h", HOLDER_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, HOLDER_CORES);
        if (not w.valid() or not h.valid())
        {
            (void)w.cancel(KOS_CANCEL_KILL, 0);
            (void)h.cancel(KOS_CANCEL_KILL, 0);
            if (w.valid())
            {
                (void)w.join(KOS_TIMEOUT_NONE);
            }
            if (h.valid())
            {
                (void)h.join(KOS_TIMEOUT_NONE);
            }
            return false;
        }
        (void)h.join(KOS_TIMEOUT_NONE);
        (void)w.join(KOS_TIMEOUT_NONE);
        *d_ns = kos::clock_now() - t0;
        return true;
    }

    // False where it printed a SKIP instead.
    bool samples_print(char const* row, char const* tag, uint64_t step, uint32_t* wait,
                       uint32_t n)
    {
        char s[192];
        if (step > COARSE_CLOCK_NS)
        {
            ksnprintf(s, sizeof(s), "  %s: SKIP (the clock steps %u us)\n", row,
                      static_cast<unsigned>(step / 1000ull));
            kos::print(s);
            return false;
        }
        if (wait == nullptr)
        {
            ksnprintf(s, sizeof(s), "  %s: SKIP (no heap for the samples)\n", row);
            kos::print(s);
            return false;
        }
        for (uint32_t i = 1; i < n; i++)
        {
            uint32_t const v = wait[i];
            uint32_t j = i;
            while (j > 0 and wait[j - 1u] > v)
            {
                wait[j] = wait[j - 1u];
                j--;
            }
            wait[j] = v;
        }
        ksnprintf(s, sizeof(s), "  %s: %u/%u/%u ns  (p50/p99/max, n=%u) [%s]\n", row,
                  static_cast<unsigned>(ranked(wait, n, 1, 2)),
                  static_cast<unsigned>(ranked(wait, n, 99, 100)),
                  static_cast<unsigned>(wait[n - 1u]), static_cast<unsigned>(n), tag);
        kos::print(s);
        return true;
    }

    void handoff_print(char const* row, char const* tag, uint64_t step, uint64_t d_ns,
                       Handoff const* ho, uint32_t* wait)
    {
        uint32_t const n = ho->taken;
        char s[192];
        if (n != ho->rounds)
        {
            ksnprintf(s, sizeof(s), "  %s: SKIP (stopped after %u of %u)\n", row,
                      static_cast<unsigned>(n), static_cast<unsigned>(ho->rounds));
            kos::print(s);
            return;
        }
        ksnprintf(s, sizeof(s), "  %s-round: %u ns/round  (%u rounds / %u ms) [%s]\n", row,
                  static_cast<unsigned>(d_ns / n), static_cast<unsigned>(n),
                  static_cast<unsigned>(d_ns / 1000000ull), tag);
        kos::print(s);
        if (not samples_print(row, tag, step, wait, n))
        {
            return;
        }
#if KICKOS_BENCH
        (void)kos_bench(KOS_BENCH_OP_DIST_PRINT, 0, 0);
#endif
    }

    char const* handoff_where()
    {
        if (WAITER_CORES != 0)
        {
            return "waiter core 1, holder core 0";
        }
        return "one core";
    }

    void pairs_tag(char* tag, size_t size, uint32_t pairs)
    {
        if (pairs == LOAD_PAIRS)
        {
            ksnprintf(tag, size, "%s; %u of %u pairs parked", handoff_where(),
                      static_cast<unsigned>(pairs), static_cast<unsigned>(LOAD_PAIRS));
        }
        else
        {
            ksnprintf(tag, size, "%s; %u of %u pairs parked, %s create %d", handoff_where(),
                      static_cast<unsigned>(pairs), static_cast<unsigned>(LOAD_PAIRS), g_load_stop,
                      g_load_err);
        }
    }

    // The same handoff while other pairs stand parked, each a holder with one waiter on its mutex.
    void handoff_loaded(uint64_t step)
    {
        uint32_t const pairs = load_up();
        uint64_t d_ns = 0;
        bool ran = false;
        if (pairs != 0)
        {
            ran = handoff_run(&g_ho, waiter, KOS_TASK_NONE, nullptr, 0, &d_ns);
        }
        load_down();
        if (pairs == 0)
        {
            kos::print("  mutex-handoff-loaded: SKIP (no load pair could spawn)\n");
            return;
        }
        if (not ran)
        {
            kos::print("  mutex-handoff-loaded: SKIP (thread pool too small)\n");
            return;
        }
        char tag[112];
        pairs_tag(tag, sizeof(tag), pairs);
        handoff_print("mutex-handoff-loaded", tag, step, d_ns, &g_ho, g_wait);
    }

#if KICKOS_HAVE_ASPACE
    // What stopped the range fill short, or "".
    char const* g_fill_stop = "";

    // Grants one-page blocks in this task's space until `left` range slots stay free.
    void ranges_fill(uint32_t left)
    {
        while (true)
        {
            uint32_t free_slots = 0;
            if (kos_mem_count(KOS_MEM_RANGES_FREE, &free_slots) != 0)
            {
                g_fill_stop = "count";
                return;
            }
            if (free_slots <= left)
            {
                return;
            }
            void* const b = kos_ram_alloc(1);
            if (b == nullptr)
            {
                g_fill_stop = "reserve";
                return;
            }
            if (kos_mem_self_grant(b, 1, 0) != 0)
            {
                g_fill_stop = "grant";
                return;
            }
        }
    }

    // The loaded handoff with the waiter in a child task, reaching the word through a memory
    // window, and this space's range list full: the holder's could-hold test scans all of it.
    void handoff_window(uint64_t step)
    {
        uint32_t const pairs = load_up();
        // Two left: the word's block, granted last so the scan meets it late, and the holder's
        // stack.
        ranges_fill(2);
        WindowBlock* const b = static_cast<WindowBlock*>(kos_ram_alloc(sizeof(WindowBlock)));
        bool ok = b != nullptr and kos_mem_self_grant(b, sizeof(WindowBlock), 0) == 0;
        uint32_t free_slots = 0;
        ok = ok and kos_mem_count(KOS_MEM_RANGES_FREE, &free_slots) == 0;
        kos_task_t task = KOS_TASK_NONE;
        ok = ok and kos_task_create(nullptr, 0, 0, &task) == 0;
        uint64_t d_ns = 0;
        bool ran = false;
        if (ok and pairs != 0)
        {
            b->ho.rounds = HANDOFFS;
            kos_window const win = {reinterpret_cast<uintptr_t>(b),
                                    static_cast<uint32_t>(sizeof(WindowBlock)), KOS_WINDOW_MEMORY,
                                    0};
            ran = handoff_run(&b->ho, window_waiter, task, &win, 1, &d_ns);
        }
        load_down();
        if (task != KOS_TASK_NONE)
        {
            (void)kos_task_slay(task, 0);
        }
        if (not ok)
        {
            kos::print("  mutex-handoff-window: SKIP (no block or no task)\n");
            return;
        }
        if (pairs == 0)
        {
            kos::print("  mutex-handoff-window: SKIP (no load pair could spawn)\n");
            return;
        }
        if (not ran)
        {
            kos::print("  mutex-handoff-window: SKIP (thread pool too small)\n");
            return;
        }
        char pt[112];
        pairs_tag(pt, sizeof(pt), pairs);
        char tag[160];
        if (g_fill_stop[0] == '\0')
        {
            ksnprintf(tag, sizeof(tag), "%s; %u of %u ranges taken", pt,
                      static_cast<unsigned>(KICKOS_ASPACE_RANGES - free_slots),
                      static_cast<unsigned>(KICKOS_ASPACE_RANGES));
        }
        else
        {
            ksnprintf(tag, sizeof(tag), "%s; %u of %u ranges taken, fill stopped at %s", pt,
                      static_cast<unsigned>(KICKOS_ASPACE_RANGES - free_slots),
                      static_cast<unsigned>(KICKOS_ASPACE_RANGES), g_fill_stop);
        }
        handoff_print("mutex-handoff-window", tag, step, d_ns, &b->ho, b->wait);
    }
#endif

    // Two threads ping-ponging on two semaphores; a sample is one round trip.
    void sem_handoff(uint64_t step)
    {
        Handoff* const ho = &g_sho;
        ho->enter = 0;
        ho->taken = 0;
        ho->rounds = g_ho.rounds;
        uint64_t const t0 = kos::clock_now();
        auto w = kos::thread::create_caps(ponger, ho, "mx_sw", WAITER_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, WAITER_CORES);
        auto h = kos::thread::create_caps(pinger, ho, "mx_sh", HOLDER_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, HOLDER_CORES);
        if (not w.valid() or not h.valid())
        {
            (void)w.cancel(KOS_CANCEL_KILL, 0);
            (void)h.cancel(KOS_CANCEL_KILL, 0);
            if (w.valid())
            {
                (void)w.join(KOS_TIMEOUT_NONE);
            }
            if (h.valid())
            {
                (void)h.join(KOS_TIMEOUT_NONE);
            }
            kos::print("  sem-handoff: SKIP (thread pool too small)\n");
            return;
        }
        (void)h.join(KOS_TIMEOUT_NONE);
        (void)w.join(KOS_TIMEOUT_NONE);
        handoff_print("sem-handoff", handoff_where(), step, kos::clock_now() - t0, ho, g_wait);
    }

    // A crowd parked on one semaphore below the poster, all on the poster's core, so a post
    // readies one of them and switches to none.
    constexpr uint32_t CROWD_ROUNDS = 200;
    static_assert(CROWD_ROUNDS <= HANDOFFS, "the crowd's samples go in g_wait");
    constexpr uint8_t SETTLE_PRIO = LOAD_PRIO - 1;
    kos_sem_t g_crowd = KOS_SEM_INIT(0);
    kos_sem_t g_settle_go = KOS_SEM_INIT(0);
    kos_sem_t g_settled = KOS_SEM_INIT(0);
    Atomic<uint32_t, Order::RELAXED> g_crowd_stop{0};
    uint32_t g_crowd_n = 0;
    uint32_t g_crowd_taken = 0;

    void crowd_waiter(void*)
    {
        while (kos_sem_wait(&g_crowd, KOS_TIMEOUT_NONE) == 0 and g_crowd_stop == 0)
        {
        }
    }

    // Below the crowd: it runs only once every crowd thread has parked again.
    void settle_witness(void*)
    {
        while (g_crowd_stop == 0)
        {
            kos_sem_wait(&g_settle_go, KOS_TIMEOUT_NONE);
            kos_sem_post(&g_settled);
        }
    }

    void crowd_settle()
    {
        kos_sem_post(&g_settle_go);
        kos_sem_wait(&g_settled, KOS_TIMEOUT_NONE);
    }

    void crowd_poster(void*)
    {
        crowd_settle();
        for (uint32_t r = 0; r < CROWD_ROUNDS; r++)
        {
            uint64_t const t0 = kos::clock_now();
            kos_sem_post(&g_crowd);
            uint64_t d = kos::clock_now() - t0;
            if (d > UINT32_MAX)
            {
                d = UINT32_MAX;
            }
            g_wait[r] = static_cast<uint32_t>(d);
            for (uint32_t i = 1; i < g_crowd_n; i++)
            {
                kos_sem_post(&g_crowd);
            }
            crowd_settle();
            g_crowd_taken = r + 1u;
        }
        g_crowd_stop = 1;
        for (uint32_t i = 0; i < g_crowd_n; i++)
        {
            kos_sem_post(&g_crowd);
        }
        kos_sem_post(&g_settle_go);
    }

    kos::thread::Handle crowd_spawn(void (*entry)(void*), char const* name, uint8_t prio)
    {
        return kos::thread::create_caps(entry, nullptr, name, prio, nullptr, 0, KOS_POLICY_FIFO,
                                        0, nullptr, 0, 0, nullptr, KOS_TASK_NONE, nullptr, 0,
                                        HOLDER_CORES);
    }

    void sem_crowd(uint64_t step)
    {
        if (g_wait == nullptr)
        {
            (void)samples_print("sem-wake-crowd", "", step, g_wait, 0);
            return;
        }
        g_crowd_stop = 0;
        g_crowd_taken = 0;
        (void)kos_sem_init(&g_crowd, 0);
        (void)kos_sem_init(&g_settle_go, 0);
        (void)kos_sem_init(&g_settled, 0);
        g_crowd_n = 0;
        auto s = crowd_spawn(settle_witness, "mx_cs", SETTLE_PRIO);
        while (s.valid() and g_crowd_n < 2 * LOAD_PAIRS + 1)
        {
            auto w = crowd_spawn(crowd_waiter, "mx_cw", LOAD_PRIO);
            if (not w.valid())
            {
                break;
            }
            g_load_t[g_crowd_n] = w;
            g_crowd_n++;
        }
        kos::thread::Handle p;
        if (s.valid())
        {
            p = crowd_spawn(crowd_poster, "mx_cp", HOLDER_PRIO);
        }
        // The crowd took every slot: one of it gives its slot to the poster.
        while (s.valid() and not p.valid() and g_crowd_n > 1)
        {
            g_crowd_n--;
            (void)g_load_t[g_crowd_n].cancel(KOS_CANCEL_KILL, 0);
            (void)g_load_t[g_crowd_n].join(KOS_TIMEOUT_NONE);
            p = crowd_spawn(crowd_poster, "mx_cp", HOLDER_PRIO);
        }
        if (p.valid())
        {
            (void)p.join(KOS_TIMEOUT_NONE);
        }
        else
        {
            g_crowd_stop = 1;
            for (uint32_t i = 0; i < g_crowd_n; i++)
            {
                kos_sem_post(&g_crowd);
            }
            kos_sem_post(&g_settle_go);
        }
        if (s.valid())
        {
            (void)s.join(KOS_TIMEOUT_NONE);
        }
        for (uint32_t i = 0; i < g_crowd_n; i++)
        {
            (void)g_load_t[i].join(KOS_TIMEOUT_NONE);
        }
        if (g_crowd_taken != CROWD_ROUNDS)
        {
            kos::print("  sem-wake-crowd: SKIP (thread pool too small)\n");
            return;
        }
        char tag[64];
        ksnprintf(tag, sizeof(tag), "%u waiters parked, one woken", static_cast<unsigned>(g_crowd_n));
        (void)samples_print("sem-wake-crowd", tag, step, g_wait, CROWD_ROUNDS);
    }

    void handoff(uint64_t step, uint32_t reps)
    {
        if (step <= COARSE_CLOCK_NS)
        {
            g_wait = static_cast<uint32_t*>(malloc(HANDOFFS * sizeof(uint32_t)));
        }
        else
        {
            g_ho.rounds = COARSE_ROUNDS;
        }
        uint64_t d_ns = 0;
        if (not handoff_run(&g_ho, waiter, KOS_TASK_NONE, nullptr, 0, &d_ns))
        {
            uncontended_print(reps);
            kos::print("  mutex-handoff: SKIP (thread pool too small)\n");
            return;
        }
        uncontended_print(reps);
        handoff_print("mutex-handoff", handoff_where(), step, d_ns, &g_ho, g_wait);
        // The rows drain on the console before the loaded window opens.
        kos::sleep_ns(QUIET_NS);
        handoff_loaded(step);
#if KICKOS_HAVE_ASPACE
        kos::sleep_ns(QUIET_NS);
        handoff_window(step);
#else
        kos::print("  mutex-handoff-window: SKIP (no address spaces)\n");
#endif
        kos::sleep_ns(QUIET_NS);
        sem_handoff(step);
        kos::sleep_ns(QUIET_NS);
        sem_crowd(step);
    }
}

namespace
{
    int finish(int status)
    {
        (void)kos_console_flush(kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
        (void)kos_shutdown(status, KOS_SHUTDOWN_BOOTLOADER);
        return status;
    }
}

int main(int, char**)
{
    (void)kos_terminal_outcome(KOS_SHUTDOWN_BOOTLOADER);
    kos::print("mutexbench: the mutex and the C library's locks, as an application pays\n");
    // The banner drains on the console before the first row starts.
    kos::sleep_ns(QUIET_NS);
    uint64_t const step = clock_step();
    uint32_t reps = REPS;
    if (step > COARSE_CLOCK_NS)
    {
        reps = COARSE_REPS;
    }
    // The first pass only warms up: on an emulator it reads up to twice the second.
    uncontended(reps);
    uncontended(reps);
    handoff(step, reps);
    free(g_wait);
    kos::print("mutexbench: done\n");
    return finish(0);
}
