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
// its granted MMIO); semaphore destroy (freelist reuse, stale-handle rejection,
// quiescent-only); a privileged guard access surviving a syscall.

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
        char const* s = "# [svc] console_write roundtrip\n";
        long r = kos_kconsole_write(s, strlen(s));
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
        int a = kos::thread::spawn(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10);
        int b = kos::thread::spawn(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10);
        TAP_CHECK(a >= 0 and b >= 0); // spawn failure (e.g. exhausted thread pool) would hang the join
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
        int hi = kos::thread::spawn(preempt_high, nullptr, "high", 20);
        int lo = kos::thread::spawn(preempt_low, nullptr, "low", 8);
        TAP_CHECK(hi >= 0 and lo >= 0); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_go); // reclaim: the suite must be pool-honest (runs on MAX_SEMAPHORES=4)
        TAP_CHECK(log_eq("lHL"));
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // The IRQ tests below drive kos_irq_inject, a KICKOS_ENABLE_SELFTEST-only
    // syscall. Without the flag inject is a kernel no-op, so registering these would
    // deadlock on a handler that never fires -- gate the definitions with the
    // registrations (main) so a plain build simply omits them.
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
        int w = kos::thread::spawn(irq_waiter, nullptr, "irqW", 15);
        int inj = kos::thread::spawn(irq_injector, nullptr, "irqI", 8);
        TAP_CHECK(w >= 0 and inj >= 0); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_irq); // reclaim (line 5 stays bound to a now-stale handle -> fails safe)
        TAP_CHECK(log_eq("iWr"));
    }

#endif // KICKOS_ENABLE_SELFTEST (IRQ-context post)

    // --- Round-robin interleave ------------------------------------------------
    // Burn target per iteration (~2 quanta); t_rr sizes it to the target's clock so
    // the slice always preempts mid-burn, coarse-clock boards included.
    uint64_t g_rr_burn_ns = 2000000ull;
    void rr_worker(void* arg)
    {
        char c = arg_char(arg);
        for (int i = 0; i < 3; i++)
        {
            log_put(c);
            // Burn longer than the slice so the timer preempts to the equal-priority
            // peer mid-run (g_rr_burn_ns is scaled to the quantum in t_rr).
            uint64_t start = kos_clock_now();
            while (kos_clock_now() - start < g_rr_burn_ns)
            {
            }
        }
        kos_sem_post(g_done);
    }
    void t_rr()
    {
        // The RR quantum must be resolvable by the monotonic clock, or the slice
        // can't preempt mid-burn and the interleave never happens. Don't assume a
        // fine clock: scale the quantum to the target's clock granule so RR is
        // exercised on EVERY board (the coarse QEMU semihosting clock included) --
        // a quantum below the clock's resolution is neither testable nor shippable.
        // Measure one full granule (two consecutive edges, phase-independent; the
        // probe spins so the clock advances, no WFI).
        uint64_t e0 = kos_clock_now();
        uint64_t e1 = e0;
        while (e1 == e0) { e1 = kos_clock_now(); }
        uint64_t e2 = e1;
        while (e2 == e1) { e2 = kos_clock_now(); }
        uint64_t granule = e2 - e1;
        uint64_t quantum = 1000000ull; // 1 ms on a fine clock (the shipped case)
        if (quantum < granule * 4)
        {
            quantum = granule * 4; // coarse clock: keep the slice well above a granule
        }
        g_rr_burn_ns = quantum * 2; // ~2 slices per burn -> guaranteed mid-burn preempt

        log_reset();
        int a = kos::thread::spawn(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                                   KOS_POLICY_RR, static_cast<uint32_t>(quantum), /*privileged=*/true);
        int b = kos::thread::spawn(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                                   KOS_POLICY_RR, static_cast<uint32_t>(quantum), /*privileged=*/true);
        TAP_CHECK(a >= 0 and b >= 0); // spawn failure would hang the join below
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
        int l = kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL", 10);
        int s = kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS", 10);
        TAP_CHECK(l >= 0 and s >= 0); // spawn failure would hang the join below
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
        int a = kos::thread::spawn(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10);
        int b = kos::thread::spawn(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10);
        // A silently-dropped spawn (e.g. an exhausted thread pool) leaves the
        // workers non-existent, so main would post to nobody and hang in wait_n --
        // fail loud here instead. (This is the XMC MAX_THREADS=8 pool-exhaustion
        // deadlock that hid behind an ignored spawn return.)
        TAP_CHECK(a >= 0 and b >= 0);
        kos_sleep_ns(5000000ull); // let both block on g_multi
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        wait_n(2);
        kos_sem_destroy(g_multi); // reclaim
        TAP_CHECK(count('A') == 1 and count('B') == 1); // both woke
    }

#if defined(KICKOS_ENABLE_SELFTEST) // inject-driven (see the tier-2 block above)
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
        // Alloc before the sems: an alloc-fail early return must not leak them (pool-honest suite).
        g_mmio = kos_ram_alloc(4096);
        TAP_CHECK(g_mmio != nullptr);
        *static_cast<volatile int*>(g_mmio) = 0;
        g_irqdrv_done = kos_sem_create(0);
        g_irqdrv_ready = kos_sem_create(0);
        int drv = kos::thread::spawn(irq_driver, nullptr, "irqdrv", 15, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false, g_mmio, 4096);
        if (drv < 0)
        {
            kos_sem_destroy(g_irqdrv_done); // reclaim before the failure return
            kos_sem_destroy(g_irqdrv_ready);
        }
        TAP_CHECK(drv >= 0); // spawn failure would hang the ready handshake below
        kos_sem_wait(g_irqdrv_ready);
        for (int i = 1; i <= 3; i++)
        {
            *static_cast<volatile int*>(g_mmio) = 0x100 + i; // "device" produces data
            kos_irq_inject(kIrqLine);
            kos_sem_wait(g_irqdrv_done); // serviced + acked
        }
        kos_sem_destroy(g_irqdrv_done); // reclaim (driver has exited: higher prio ran to completion)
        kos_sem_destroy(g_irqdrv_ready);
        TAP_CHECK(g_seen[0] == 0x101 and g_seen[1] == 0x102 and g_seen[2] == 0x103);
    }

    // --- IRQ mask actually drops a masked raise --------------------------------
    // The driver runs BELOW root's priority, so posting its notification does not
    // preempt root: root can fire twice back-to-back. The first fire masks the
    // line; the second must be dropped (masked), so exactly one service results.
    // After ack (unmask) a further fire delivers again.
    int g_mask_ready = -1;
    int g_mask_serviced = 0;
    constexpr int kMaskLine = 6;

    void mask_driver(void*)
    {
        auto irq = kos::Irq::request(kMaskLine);
        kos_sem_post(g_mask_ready);
        for (int i = 0; i < 2; i++)
        {
            irq.wait();
            g_mask_serviced++;
            irq.ack();
            kos_sem_post(g_done);
        }
    }
    void t_irq_mask()
    {
        g_mask_ready = kos_sem_create(0);
        g_mask_serviced = 0;
        int drv = kos::thread::spawn(mask_driver, nullptr, "maskdrv", 1); // below root (prio 2)
        TAP_CHECK(drv >= 0);        // spawn failure would hang the ready handshake below
        kos_sem_wait(g_mask_ready); // line registered
        kos_sem_destroy(g_mask_ready);                          // handshake done -> reclaim
        kos_irq_inject(kMaskLine);                              // fire 1: ISR masks + posts
        kos_irq_inject(kMaskLine);                              // fire 2: line masked -> dropped
        wait_n(1);
        TAP_CHECK(g_mask_serviced == 1); // the masked second raise was dropped
        kos_irq_inject(kMaskLine);       // unmasked after ack -> delivers
        wait_n(1);
        TAP_CHECK(g_mask_serviced == 2);
    }

    // --- Auto-rearm: wait; service with NO explicit ack ------------------------
    // irq_wait re-arms the previously-consumed line itself, so a driver that never
    // acks still receives every subsequent IRQ. Driver runs ABOVE root (like
    // t_irqdrv) so it re-arms (reaches its next wait) before root injects again --
    // no masked-line window (a masked inject would be dropped, sim/ARM alike).
    int g_autorearm_ready = -1;
    int g_autorearm_seen = 0;
    constexpr int kAutoRearmLine = 8;

    void autorearm_driver(void*)
    {
        auto irq = kos::Irq::request(kAutoRearmLine);
        kos_sem_post(g_autorearm_ready);
        for (int i = 0; i < 3; i++)
        {
            irq.wait(); // no ack: the next wait re-arms the line on its own
            g_autorearm_seen++;
            kos_sem_post(g_done);
        }
    }
    void t_irq_autorearm()
    {
        g_autorearm_ready = kos_sem_create(0);
        g_autorearm_seen = 0;
        int drv = kos::thread::spawn(autorearm_driver, nullptr, "autoirq", 15);
        TAP_CHECK(drv >= 0); // spawn failure would hang the ready handshake below
        kos_sem_wait(g_autorearm_ready);
        kos_sem_destroy(g_autorearm_ready);
        for (int i = 0; i < 3; i++)
        {
            kos_irq_inject(kAutoRearmLine);
            wait_n(1);
        }
        TAP_CHECK(g_autorearm_seen == 3); // all three delivered without a single ack
    }

    // --- Pitfall-1 regression: no phantom wake in the ack;compute;wait shape ----
    // After an explicit ack re-arms the line, exactly ONE injected event must
    // yield exactly ONE wait-return: the second wait BLOCKS. A variant that sets
    // needs_rearm in the ISR (instead of on wait-return) would unmask early and
    // phantom-post, leaving the driver to service an event that never came. Driver
    // runs BELOW root so root sequences each step; every inject targets an armed
    // line (register / explicit ack / rearm-at-wait), never a masked one.
    int g_phantom_ready = -1;
    int g_phantom_seen = 0;
    constexpr int kPhantomLine = 10;

    void phantom_driver(void*)
    {
        auto irq = kos::Irq::request(kPhantomLine);
        kos_sem_post(g_phantom_ready);
        irq.wait();           // consume fire #1 -> needs_rearm set, line masked
        irq.ack();            // ack;compute;wait shape: unmask now, needs_rearm clear
        kos_sem_post(g_done); // acked; root injects the one mid-compute event
        irq.wait();           // consume that one event
        g_phantom_seen++;
        kos_sem_post(g_done);
        irq.wait();           // MUST block: only one event was injected, no phantom
        g_phantom_seen++;     // reached only on a phantom wake (the bug)
        kos_sem_post(g_done);
    }
    void t_irq_phantom()
    {
        g_phantom_ready = kos_sem_create(0);
        g_phantom_seen = 0;
        int drv = kos::thread::spawn(phantom_driver, nullptr, "phantirq", 1); // below root
        TAP_CHECK(drv >= 0); // spawn failure would hang the ready handshake below
        kos_sem_wait(g_phantom_ready);
        kos_sem_destroy(g_phantom_ready);

        kos_irq_inject(kPhantomLine); // fire #1
        wait_n(1);                    // driver consumed #1 and acked (line armed)

        kos_irq_inject(kPhantomLine); // the one mid-compute event, on the armed line
        wait_n(1);                    // serviced exactly once
        TAP_CHECK(g_phantom_seen == 1);

        // The driver is now parked in its third wait. It is lower priority, so
        // sleeping yields the CPU to it: a phantom wake would bump seen here.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_phantom_seen == 1); // second wait genuinely blocked -> no phantom

        // Prove that wait is live (blocked, not lost) and the line re-armed itself:
        // a fresh inject delivers.
        kos_irq_inject(kPhantomLine);
        wait_n(1);
        TAP_CHECK(g_phantom_seen == 2);
    }

#endif // KICKOS_ENABLE_SELFTEST (tier-1 IRQ + mask)

    // --- Semaphore destroy: freelist reuse + generation-tagged handles ---------
    void t_sem_destroy()
    {
        int h = kos_sem_create(0);
        TAP_CHECK(h >= 0);
        TAP_CHECK(kos_sem_destroy(h) == 0);  // live handle destroys
        TAP_CHECK(kos_sem_destroy(h) == -1); // stale handle rejected (gen bumped)
        int h2 = kos_sem_create(0);
        TAP_CHECK(h2 >= 0 and h2 != h); // reused slot carries a fresh generation
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        // Malformed / out-of-range handles at the resolve boundary must fail-safe (the
        // M2 capability model builds on sem_resolve): negative, garbage-huge, and an
        // out-of-range index all reject. Via sem_destroy (the one sem syscall that
        // returns a value; wait/post share the same sem_resolve chokepoint). Pool-neutral.
        TAP_CHECK(kos_sem_destroy(-1) == -1);
        TAP_CHECK(kos_sem_destroy(0x7fffffff) == -1);
        TAP_CHECK(kos_sem_destroy(0x00ffffff) == -1);
    }

    // --- Semaphore destroy is quiescent-only (refused while a waiter is parked) -
    int g_dsem = -1;
    void destroy_waiter(void*)
    {
        kos_sem_wait(g_dsem); // parks (initial 0)
        kos_sem_post(g_done);
    }
    void t_sem_destroy_busy()
    {
        g_dsem = kos_sem_create(0);
        int w = kos::thread::spawn(destroy_waiter, nullptr, "dwaiter", 15);
        TAP_CHECK(w >= 0);                         // spawn failure would hang wait_n(1) below
        kos_sleep_ns(2000000ull);                 // let it block on g_dsem
        TAP_CHECK(kos_sem_destroy(g_dsem) == -1); // has a waiter -> refused
        kos_sem_post(g_dsem);                     // release the waiter
        wait_n(1);
        TAP_CHECK(kos_sem_destroy(g_dsem) == 0); // quiescent -> destroyed
    }

    // --- Owning kos::Semaphore RAII --------------------------------------------
    void t_sem_raii()
    {
        // Scoped create/destroy far exceeding the pool size must not exhaust it:
        // the old non-owning dtor leaked, so this would fail after ~16.
        for (int i = 0; i < 100; i++)
        {
            kos::Semaphore s;
            TAP_CHECK(s.id() >= 0);
        }
        // Move-construct empties the source, so scope exit destroys once.
        kos::Semaphore a;
        int aid = a.id();
        kos::Semaphore b(static_cast<kos::Semaphore&&>(a));
        TAP_CHECK(b.id() == aid and a.id() < 0);

        // Move-assign onto a live handle: the old target is destroyed, source emptied.
        kos::Semaphore c;
        c = static_cast<kos::Semaphore&&>(b);
        TAP_CHECK(c.id() == aid and b.id() < 0);

        // Self-move-assign is a no-op (must not destroy its own handle). Aliased
        // through a reference so the compiler's -Wself-move doesn't fire.
        kos::Semaphore& cref = c;
        c = static_cast<kos::Semaphore&&>(cref);
        TAP_CHECK(c.id() == aid);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
#if KICKOS_HAVE_MPU
    // --- Privileged guard access survives a syscall ----------------------------
    // Needs enforced protection: kos_guard_addr returns a real guarded page only
    // where a wild access faults (sim now; per chip at M2). On a board without it
    // the probe is 0 and this would fault, so it is compiled out there.
    void t_mpu_guard()
    {
        volatile int* g = static_cast<volatile int*>(kos_guard_addr());
        *g = 0x1234; // privileged (root): guard is RW, must not fault
        kos_yield(); // a syscall must restore the caller's MPU posture, not PROT_NONE
        TAP_CHECK(*g == 0x1234);
    }
#endif

    // --- One driver per line: a second claim on a bound line is refused --------
    void t_irq_ownership()
    {
        constexpr int kLine = 11; // unused by the other IRQ tests
        int sem = kos_sem_create(0);
        TAP_CHECK(kos_irq_attach(kLine, sem) == 0);  // first claim wins
        TAP_CHECK(kos_irq_attach(kLine, sem) == -1); // second is refused (no steal)
        TAP_CHECK(kos_irq_register(kLine) == -1);    // tier-1 cannot steal it either
        kos_sem_destroy(sem); // reclaim (line 11 stays bound to a now-stale handle -> fails safe)
    }

    // --- Spurious IRQ: an unbound line is masked + counted, never dropped -------
    void t_irq_spurious()
    {
        constexpr int kFreeLine = 9; // no driver bound to this line
        // Enable the line so the injected raise reaches the default handler on
        // masked-by-default controllers (ARM NVIC, RX); sim/riscv are unmasked by
        // default, so this is a no-op there.
        kos_irq_unmask(kFreeLine);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(kFreeLine);   // default handler runs: mask + bump counter
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        // The default handler masked the line, so a second raise is dropped: the
        // counter must NOT advance again (proves it was masked, not re-delivered).
        kos_irq_inject(kFreeLine);
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
    }
#endif

    // --- Caller-owned thread stack: spawn takes a caller-provided stack (and rejects an
    // undersized/misaligned one) -- a thread's stack is a userspace concern (M1). ---------
    int g_cstk_sem = -1;
    void caller_stack_worker(void*) { kos_sem_post(g_cstk_sem); } // ran on the caller's stack
    // Statically-defined caller-owned stack (the KOS_STACK_DEFINE shape), exercised on
    // no-MPU builds only -- this is the path that regressed: KOS_STACK_DEFINE aligns to 16
    // without an MPU, which the kernel's (formerly ungated) stack natural-alignment check
    // then rejected. Under MPU the macro naturally-aligns to a full region (a page on the
    // sim backend), so the static buffer would not fit a small-appdata enforcement chip's
    // fixed .appdata window (e.g. C6 = 4K); the MPU caller-owned-stack path is covered by
    // the dynamic alloc'd stack above.
#if !KICKOS_HAVE_MPU
    KOS_STACK_DEFINE(g_cstk_static, 512);
#endif
    void t_caller_stack()
    {
        // Reject a non-null, tiny + misaligned caller stack: must fail, not run or corrupt.
        TAP_CHECK(kos::thread::spawn(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8) < 0);
        // Accept a properly-sized, aligned caller-owned stack -> the thread runs on it. Skip
        // (still ok) when the arena can't spare one (tiny-RAM parts, like test 11's alloc):
        // the API is arch-uniform; this only needs the memory to demonstrate it.
        constexpr uint32_t kStk = 2048;
        void* raw = kos_ram_alloc(kStk + 16);
        if (raw == nullptr)
        {
            return;
        }
        void* stk = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
        g_cstk_sem = kos_sem_create(0);
        int const t = kos::thread::spawn(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, stk, kStk);
        TAP_CHECK(t >= 0);        // spawn accepted the caller-owned stack
        kos_sem_wait(g_cstk_sem); // the worker ran on it and posted
        kos_sem_destroy(g_cstk_sem);
#if !KICKOS_HAVE_MPU
        // Same shape via a statically-defined KOS_STACK_DEFINE buffer, unprivileged. This
        // buffer is only 16-byte aligned (no MPU); spawn must still accept + run it -- the
        // path that regressed, since with no region descriptor the natural-alignment check
        // must not apply.
        g_cstk_sem = kos_sem_create(0);
        int const ts = kos::thread::spawn(caller_stack_worker, nullptr, "cstkS", 10, KOS_POLICY_FIFO,
                                          0, false, nullptr, 0, g_cstk_static,
                                          static_cast<uint32_t>(sizeof(g_cstk_static)));
        TAP_CHECK(ts >= 0);       // spawn accepted the static caller-owned stack
        kos_sem_wait(g_cstk_sem); // the worker ran on it and posted
        kos_sem_destroy(g_cstk_sem);
#endif
    }

    // --- Memory domains: two unprivileged threads granted the SAME region share one
    // domain -- each reads/writes it -- while each keeps its own private stack. The
    // negative half (a cross-domain write faults) is the standalone mpu_fault app. ---
    volatile int* g_dshared = nullptr;
    int g_dwrote = -1;      // writer -> reader handoff (through the shared domain)
    int g_dread = -1;       // reader -> main handoff
    int g_dreadback = -1;
    constexpr int kDomSentinel = 0x5A5A;
    void dom_writer(void*)
    {
        *g_dshared = kDomSentinel; // write the shared domain region (granted)
        kos_sem_post(g_dwrote);
    }
    void dom_reader(void*)
    {
        kos_sem_wait(g_dwrote);   // after the writer stored the sentinel
        g_dreadback = *g_dshared; // read the SAME region -> proves the shared grant
        kos_sem_post(g_dread);
    }
    void t_domain_share()
    {
        // Skip (still ok) on a part whose arena cannot spare the region (like the
        // caller-stack test). Alloc before the sems so an early return leaks nothing.
        g_dshared = static_cast<volatile int*>(kos_ram_alloc(256));
        if (g_dshared == nullptr)
        {
            return;
        }
        *g_dshared = 0;
        g_dwrote = kos_sem_create(0);
        g_dread = kos_sem_create(0);
        // Spawn BOTH before either runs (spawn does not preempt): same mem_base =>
        // they reference the ONE shared domain concurrently, each with its own stack.
        int w = kos::thread::spawn(dom_writer, nullptr, "domW", 10, KOS_POLICY_FIFO, 0,
                                   false, const_cast<int*>(g_dshared), 256);
        int r = kos::thread::spawn(dom_reader, nullptr, "domR", 10, KOS_POLICY_FIFO, 0,
                                   false, const_cast<int*>(g_dshared), 256);
        if (w < 0 or r < 0)
        {
            // A tiny thread pool (microbit MAX_THREADS=2, with a low-prio driver from
            // an earlier stage still parked) cannot host both workers concurrently:
            // skip (still ok). sim + qemu (larger pools) exercise the shared domain.
            // Any worker that did spawn self-completes (writes, posts, returns->exits).
            kos::print("# domain_share: SKIP (thread pool too small for 2 concurrent)\n");
            kos_sem_destroy(g_dwrote);
            kos_sem_destroy(g_dread);
            return;
        }
        kos_sem_wait(g_dread); // the reader saw the writer's store via the shared region
        kos_sem_destroy(g_dwrote);
        kos_sem_destroy(g_dread);
        TAP_CHECK(g_dreadback == kDomSentinel);
    }

    // --- MMIO grant boundary (task #9): privileged-only + encodable-only ---------
    // An MMIO grant is a capability, so the boundary REJECTS two ways (no real device
    // is mapped here -- the positive grant is HW-only, Stage 2):
    //   * a window one MPU descriptor cannot cover exactly (rounding would over-grant
    //     the neighbouring registers), and
    //   * any grant attempted by an UNPRIVILEGED caller (else a user thread maps
    //     arbitrary peripheral space and defeats isolation).
    // The sim's arch_mpu_region_encodable is fail-closed (always false), so both halves
    // land as a -1 spawn there; on an enforcing MCU the first still rejects the
    // non-encodable window and the second the privilege violation.
    int g_mmio_unpriv_rc = -2;
    int g_mmio_done = -1;
    void mmio_noop(void*) {}
    void mmio_unpriv_worker(void*)
    {
        // Unprivileged caller: the privilege gate must refuse the MMIO grant.
        g_mmio_unpriv_rc = kos::thread::spawn(mmio_noop, nullptr, "mmiochild", 10,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                              nullptr, 0, reinterpret_cast<void*>(0x1000u), 4096);
        kos_sem_post(g_mmio_done);
    }
    void t_mmio_grant()
    {
        // Privileged caller, non-encodable window (size 1, unaligned base): rejected,
        // not rounded.
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmiobad", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x1001u), 1) < 0);
        // A non-null base with size 0 is rejected at the boundary (before domain_for).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmio0", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x2000u), 0) < 0);
        // A window whose base+size wraps the address space is rejected (32-bit MCU;
        // on the 64-bit sim the fail-closed encoder rejects it first -- either way -1).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmioW2", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0xFFFFFFF0u), 0x20) < 0);
        g_mmio_done = kos_sem_create(0);
        g_mmio_unpriv_rc = -2;
        int w = kos::thread::spawn(mmio_unpriv_worker, nullptr, "mmioW", 10);
        if (w < 0)
        {
            // Tiny thread pool (e.g. microbit MAX_THREADS=2): skip the unpriv half.
            kos::print("# mmio_grant: SKIP unpriv half (thread pool too small)\n");
            kos_sem_destroy(g_mmio_done);
            return;
        }
        kos_sem_wait(g_mmio_done);
        kos_sem_destroy(g_mmio_done);
        TAP_CHECK(g_mmio_unpriv_rc < 0);
    }

    // --- Confused-deputy readable-buffer floor ---------------------------------
    // syscall_dispatch runs privileged and bypasses the MPU, so a user pointer it
    // READS (the kconsole_write buffer, a thread name) must lie in memory the
    // UNPRIVILEGED caller could itself reach. A rodata string literal lives in the
    // app's code/rodata (a real MPU region on HW, the host image on the sim) and
    // MUST be accepted; a pointer into no granted region (the un-owned guard page)
    // MUST be rejected, never read. All checks run from an UNPRIVILEGED worker
    // (main is privileged and bypasses the floor).
    char const kCdLit[] = "# [confdep] unpriv rodata literal reaches the console\n";
    long g_cd_lit_rc = -99;    // worker: kconsole_write(rodata literal) -> expect len
    int g_cd_goodspawn = -99;  // worker: spawn rc of a child NAMED from .rodata
    int g_cd_goodname_ran = 0; // that child ran (name-copy path did not break spawn)
    int g_cd_kidsem = -1;      // grandchild -> worker handoff
    int g_cd_done = -1;        // worker -> main
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    int g_cd_neg_ran = 0;       // the negative half actually ran (guard page available)
    long g_cd_bad_rc = -99;     // worker: kconsole_write(guard page) -> expect 0 (rejected)
    int g_cd_badname_spawn = -99; // spawn rc with a BOGUS name pointer -> expect >= 0
    int g_cd_badname_ran = 0;   // that child ran (kernel walked the bad name safely)
#endif
    void cd_kid(void* arg)
    {
        *static_cast<int*>(arg) = 1;
        kos_sem_post(g_cd_kidsem);
    }
    void cd_worker(void*) // UNPRIVILEGED
    {
        g_cd_lit_rc = kos_kconsole_write(kCdLit, strlen(kCdLit)); // rodata: accepted

        g_cd_kidsem = kos_sem_create(0);
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        g_cd_goodspawn = kos::thread::spawn(cd_kid, &g_cd_goodname_ran, "cdgood", 9);
        if (g_cd_goodspawn >= 0)
        {
            kos_sem_wait(g_cd_kidsem);
        }
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        void* bad = kos_guard_addr(); // an arena page granted to no domain
        if (bad != nullptr)
        {
            // Bogus console buffer: rejected -> 0, and never read (a wrong-accept would
            // return 8, having read the guard page the caller cannot reach).
            g_cd_bad_rc = kos_kconsole_write(bad, 8);
            // Bogus NAME pointer: the kernel must bound the walk (no fault), drop the
            // name, and still spawn the child.
            g_cd_badname_spawn = kos::thread::spawn(cd_kid, &g_cd_badname_ran,
                                                    static_cast<char const*>(bad), 9);
            if (g_cd_badname_spawn >= 0)
            {
                kos_sem_wait(g_cd_kidsem);
            }
            g_cd_neg_ran = 1;
        }
#endif
        kos_sem_destroy(g_cd_kidsem);
        kos_sem_post(g_cd_done);
    }
    void t_confused_deputy()
    {
        g_cd_done = kos_sem_create(0);
        int w = kos::thread::spawn(cd_worker, nullptr, "cdwork", 10);
        if (w < 0)
        {
            kos::print("# confused_deputy: SKIP (thread pool too small)\n");
            kos_sem_destroy(g_cd_done);
            return;
        }
        kos_sem_wait(g_cd_done);
        kos_sem_destroy(g_cd_done);
        // Positive (every backend): an unprivileged rodata literal reached the console.
        TAP_CHECK(g_cd_lit_rc == static_cast<long>(sizeof(kCdLit) - 1));
        // Positive: a child named from .rodata spawned and ran (the name-copy path works).
        TAP_CHECK(g_cd_goodspawn >= 0 and g_cd_goodname_ran == 1);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        // Negative (enforcing backend): a bogus buffer/name is rejected, never read,
        // and never faults the kernel.
        if (g_cd_neg_ran)
        {
            TAP_CHECK(g_cd_bad_rc == 0);
            TAP_CHECK(g_cd_badname_spawn >= 0 and g_cd_badname_ran == 1);
        }
#endif
    }
}

int main(int, char**)
{
    g_lock = kos_sem_create(1);
    g_done = kos_sem_create(0);

    // Core scheduler / sync / time -- no test-only syscalls, runs on every board.
    tap::add("svc_roundtrip", t_svc);
    tap::add("fifo_order", t_fifo);
    tap::add("preempt_on_ready", t_preempt);
    tap::add("rr_interleave", t_rr);
    tap::add("sleep_order", t_sleep);
    tap::add("multi_wait", t_multi);
    tap::add("sem_destroy", t_sem_destroy);
    tap::add("sem_destroy_quiescent", t_sem_destroy_busy);
    tap::add("sem_raii", t_sem_raii);
#if defined(KICKOS_ENABLE_SELFTEST)
    // Need the software-inject syscall (compiled out of the production ABI).
    tap::add("irq_thread_ctx", t_irq);
    tap::add("irq_as_event", t_irqdrv);
    tap::add("irq_mask_drop", t_irq_mask);
    tap::add("irq_autorearm", t_irq_autorearm);
    tap::add("irq_phantom_wake", t_irq_phantom);
    tap::add("irq_ownership", t_irq_ownership);
    tap::add("irq_spurious", t_irq_spurious);
#if KICKOS_HAVE_MPU
    tap::add("mpu_privileged_guard", t_mpu_guard); // needs enforced protection
#endif
#endif
    tap::add("caller_stack", t_caller_stack); // caller-owned stack API (no test-only syscalls)
    tap::add("domain_share", t_domain_share); // two threads share one memory domain
    tap::add("mmio_grant", t_mmio_grant);     // MMIO-grant boundary: privileged-only + encodable-only
    tap::add("confused_deputy", t_confused_deputy); // readable-buffer/name floor (accept rodata, reject bogus)

    // Every test joins its workers, so main returns as the last live thread:
    // the failure count becomes the process exit status (0 == all passed).
    return tap::run_all();
}
