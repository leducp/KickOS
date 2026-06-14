// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS M0 self-test (unprivileged userspace, C++). The CI gate: every M0
// verification bullet as a TAP test that self-asserts its invariant, emitting
// `ok`/`not ok` over the console (tests/tap). Ordering-sensitive stages capture
// execution order in a semaphore-locked event log and assert on it, instead of
// matching console text. The deliberate cross-domain MPU fault is a separate
// binary (apps/mpu_fault) because it ends the process.
//
// Covered: SVC roundtrip; two-thread FIFO order; higher-prio preempt on a
// thread-ctx sem post; a sem posted from an IRQ handler (IRQ ctx); RR interleave
// of equal-prio threads; tickless sleep ordering; two threads blocking on one
// sem (wait-queue regression); tier-1 IRQ-as-event (unprivileged driver reads
// its granted MMIO); a privileged guard access surviving a syscall.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/string.h>

#include "tap.h"

namespace
{
    int g_done = -1; // shared completion counter
    int g_lock = -1; // binary semaphore = mutex over the event log

    // Execution-order log: workers append a token under g_lock (race-free across
    // preemption); the orchestrator asserts on it once they have all finished.
    char g_log[128];
    int g_logn = 0;

    void log_reset()
    {
        g_logn = 0;
        g_log[0] = 0;
    }

    void log_put(char c)
    {
        kos_sem_wait(g_lock);
        if (g_logn < static_cast<int>(sizeof(g_log)) - 1)
        {
            g_log[g_logn++] = c;
            g_log[g_logn] = 0;
        }
        kos_sem_post(g_lock);
    }

    bool log_eq(char const* s)
    {
        return strlen(s) == static_cast<size_t>(g_logn) and memcmp(g_log, s, g_logn) == 0;
    }

    int count(char c)
    {
        int n = 0;
        for (int i = 0; i < g_logn; i++)
        {
            if (g_log[i] == c)
            {
                n++;
            }
        }
        return n;
    }

    // Index of the k-th (1-based) occurrence of c, or a large sentinel so that a
    // "not found" makes any `<` ordering assertion fail.
    int nth(char c, int k)
    {
        int seen = 0;
        for (int i = 0; i < g_logn; i++)
        {
            if (g_log[i] == c)
            {
                seen++;
                if (seen == k)
                {
                    return i;
                }
            }
        }
        return 1 << 30;
    }

    void wait_n(int n)
    {
        for (int i = 0; i < n; i++)
        {
            kos_sem_wait(g_done);
        }
    }

    char arg_char(void* arg)
    {
        return static_cast<char>(reinterpret_cast<uintptr_t>(arg));
    }

    // --- SVC roundtrip ---------------------------------------------------------
    void t_svc()
    {
        char const* s = "# [svc] write roundtrip\n";
        long r = kos_write(1, s, strlen(s));
        TAP_CHECK(r == static_cast<long>(strlen(s)));
    }

    // --- FIFO ordering ---------------------------------------------------------
    void fifo_worker(void* arg)
    {
        log_put(arg_char(arg));
        kos_sem_post(g_done);
    }
    void t_fifo()
    {
        log_reset();
        kos::thread::spawn(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10);
        kos::thread::spawn(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10);
        wait_n(2);
        TAP_CHECK(log_eq("AB")); // A (spawned first, equal prio) runs to completion first
    }

    // --- Priority preempt on ready (thread-ctx sem post) -----------------------
    int g_go = -1;
    void preempt_high(void*)
    {
        kos_sem_wait(g_go);
        log_put('H');
        kos_sem_post(g_done);
    }
    void preempt_low(void*)
    {
        log_put('l');
        kos_sem_post(g_go); // wakes higher-prio 'high' -> preempts now
        log_put('L');
        kos_sem_post(g_done);
    }
    void t_preempt()
    {
        log_reset();
        g_go = kos_sem_create(0);
        kos::thread::spawn(preempt_high, nullptr, "high", 20);
        kos::thread::spawn(preempt_low, nullptr, "low", 8);
        wait_n(2);
        TAP_CHECK(log_eq("lHL"));
    }

    // --- IRQ-context post (tier 2) ---------------------------------------------
    int g_irq = -1;
    void irq_waiter(void*)
    {
        kos_sem_wait(g_irq);
        log_put('W');
        kos_sem_post(g_done);
    }
    void irq_injector(void*)
    {
        log_put('i');
        kos_irq_inject(5); // ISR posts g_irq -> higher-prio waiter preempts
        log_put('r');
        kos_sem_post(g_done);
    }
    void t_irq()
    {
        log_reset();
        g_irq = kos_sem_create(0);
        kos_irq_attach(5, g_irq);
        kos::thread::spawn(irq_waiter, nullptr, "irqW", 15);
        kos::thread::spawn(irq_injector, nullptr, "irqI", 8);
        wait_n(2);
        TAP_CHECK(log_eq("iWr"));
    }

    // --- Round-robin interleave ------------------------------------------------
    void rr_worker(void* arg)
    {
        char c = arg_char(arg);
        for (int i = 0; i < 3; i++)
        {
            log_put(c);
            // Burn ~2 ms, longer than the 1 ms slice, so the timer preempts to
            // the equal-priority peer mid-run.
            uint64_t start = kos_clock_now();
            while (kos_clock_now() - start < 2000000ull)
            {
            }
        }
        kos_sem_post(g_done);
    }
    void t_rr()
    {
        log_reset();
        kos::thread::spawn(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                           KOS_POLICY_RR, 1000000u, /*privileged=*/true);
        kos::thread::spawn(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                           KOS_POLICY_RR, 1000000u, /*privileged=*/true);
        wait_n(2);
        // Sustained interleave: each of B's earlier iterations precedes A's next
        // (a pure-FIFO scheduler would run A's three to completion first).
        TAP_CHECK(count('A') == 3 and count('B') == 3);
        TAP_CHECK(nth('B', 1) < nth('A', 2));
        TAP_CHECK(nth('B', 2) < nth('A', 3));
    }

    // --- Sleep ordering (tickless timer) ---------------------------------------
    void sleeper(void* arg)
    {
        unsigned ms = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        kos_sleep_ns(static_cast<uint64_t>(ms) * 1000000ull);
        char c = 'L';
        if (ms < 20)
        {
            c = 'S';
        }
        log_put(c);
        kos_sem_post(g_done);
    }
    void t_sleep()
    {
        log_reset();
        kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL", 10);
        kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS", 10);
        wait_n(2);
        TAP_CHECK(log_eq("SL")); // the short sleeper wakes first
    }

    // --- Two equal-priority threads blocking on one semaphore ------------------
    // Regression: the blocker must detach from the ready list before parking on
    // the wait queue (shared link node); without it the second waiter is orphaned
    // and never wakes.
    int g_multi = -1;
    void multi_worker(void* arg)
    {
        kos_sem_wait(g_multi);
        log_put(arg_char(arg));
        kos_sem_post(g_done);
    }
    void t_multi()
    {
        log_reset();
        g_multi = kos_sem_create(0);
        kos::thread::spawn(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10);
        kos::thread::spawn(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10);
        kos_sleep_ns(5000000ull); // let both block on g_multi
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        wait_n(2);
        TAP_CHECK(count('A') == 1 and count('B') == 1); // both woke
    }

    // --- Tier-1 IRQ-as-event: unprivileged userspace driver --------------------
    int g_irqdrv_done = -1;
    int g_irqdrv_ready = -1;
    void* g_mmio = nullptr; // fake device MMIO word, granted to the driver
    int g_seen[3] = {0, 0, 0};
    constexpr int kIrqLine = 7;

    void irq_driver(void*)
    {
        auto irq = kos::Irq::request(kIrqLine);
        kos_sem_post(g_irqdrv_ready); // registered + about to park: safe to fire
        for (int i = 0; i < 3; i++)
        {
            irq.wait();                                      // parks in thread ctx
            g_seen[i] = *static_cast<volatile int*>(g_mmio); // read granted MMIO
            irq.ack();                                       // unmask
            kos_sem_post(g_irqdrv_done);
        }
    }
    void t_irqdrv()
    {
        g_irqdrv_done = kos_sem_create(0);
        g_irqdrv_ready = kos_sem_create(0);
        g_mmio = kos_ram_alloc(4096);
        TAP_CHECK(g_mmio != nullptr);
        *static_cast<volatile int*>(g_mmio) = 0;
        kos::thread::spawn(irq_driver, nullptr, "irqdrv", 15, KOS_POLICY_FIFO, 0,
                           /*privileged=*/false, g_mmio, 4096);
        kos_sem_wait(g_irqdrv_ready);
        for (int i = 1; i <= 3; i++)
        {
            *static_cast<volatile int*>(g_mmio) = 0x100 + i; // "device" produces data
            kos_irq_inject(kIrqLine);
            kos_sem_wait(g_irqdrv_done); // serviced + acked
        }
        TAP_CHECK(g_seen[0] == 0x101 and g_seen[1] == 0x102 and g_seen[2] == 0x103);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Privileged guard access survives a syscall ----------------------------
    void t_mpu_guard()
    {
        volatile int* g = static_cast<volatile int*>(kos_guard_addr());
        *g = 0x1234; // privileged (root): guard is RW, must not fault
        kos_yield(); // a syscall must restore the caller's MPU posture, not PROT_NONE
        TAP_CHECK(*g == 0x1234);
    }
#endif
}

extern "C" void kickos_app_main(void)
{
    g_lock = kos_sem_create(1);
    g_done = kos_sem_create(0);

    tap::add("svc_roundtrip", t_svc);
    tap::add("fifo_order", t_fifo);
    tap::add("preempt_on_ready", t_preempt);
    tap::add("irq_thread_ctx", t_irq);
    tap::add("rr_interleave", t_rr);
    tap::add("sleep_order", t_sleep);
    tap::add("multi_wait", t_multi);
    tap::add("irq_as_event", t_irqdrv);
#if defined(KICKOS_ENABLE_SELFTEST)
    tap::add("mpu_privileged_guard", t_mpu_guard);
#endif

    tap::run_all();
    // root returns -> every test thread has exited -> clean shutdown.
}
