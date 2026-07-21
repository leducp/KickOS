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
    int g_done = -1; // shared completion counter (MAIN's cap; delegated to workers)
    int g_lock = -1; // binary semaphore = mutex over the event log (MAIN's cap)

    // B1 well-known child cap indices. A fresh child table has cap-gen 0, so a delegated
    // cap's handle value == its table index; delegated cap i lands at index i+1 (index 0
    // reserved). MAIN owns g_done/g_lock and delegates them per spawn in a fixed order so
    // the shared worker helpers below can name them by these constants.
    constexpr int CH_DONE = 1; // completion counter, delegated FIRST to every worker
    constexpr int CH_LOCK = 2; // event-log mutex, delegated SECOND (logging workers only)
    constexpr int CH_AUX = 3;  // test-specific third cap (g_go / g_multi / g_irq)
    constexpr int CH_READY = 2; // handshake "ready" cap for the IRQ-driver tests (done@1, ready@2)
    constexpr uint8_t CH_FULL =
        KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER; // full-rights delegation

    // Execution-order log: workers append a token under g_lock (race-free across
    // preemption); the orchestrator asserts on it once they have all finished.
    char g_log[128];
    int g_logn = 0;

    void log_reset()
    {
        g_logn = 0;
        g_log[0] = 0;
    }

    // Called only from worker threads: names the log mutex by its delegated child cap.
    void log_put(char c)
    {
        kos_sem_wait(CH_LOCK);
        if (g_logn < static_cast<int>(sizeof(g_log)) - 1)
        {
            g_log[g_logn++] = c;
            g_log[g_logn] = 0;
        }
        kos_sem_post(CH_LOCK);
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
        kos_sem_post(CH_DONE);
    }
    void t_fifo()
    {
        log_reset();
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // -> done@1, lock@2
        int a = kos::thread::spawn_caps(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10,
                                        caps, 2);
        int b = kos::thread::spawn_caps(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10,
                                        caps, 2);
        TAP_CHECK(a >= 0 and b >= 0); // spawn failure (e.g. exhausted thread pool) would hang the join
        wait_n(2);
        TAP_CHECK(log_eq("AB")); // A (spawned first, equal prio) runs to completion first
    }

    // --- Priority preempt on ready (thread-ctx sem post) -----------------------
    int g_go = -1;
    void preempt_high(void*)
    {
        kos_sem_wait(CH_AUX); // g_go
        log_put('H');
        kos_sem_post(CH_DONE);
    }
    void preempt_low(void*)
    {
        log_put('l');
        kos_sem_post(CH_AUX); // g_go: wakes higher-prio 'high' -> preempts now
        log_put('L');
        kos_sem_post(CH_DONE);
    }
    void t_preempt()
    {
        log_reset();
        g_go = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_go, CH_FULL}};
        int hi = kos::thread::spawn_caps(preempt_high, nullptr, "high", 20, caps, 3);
        int lo = kos::thread::spawn_caps(preempt_low, nullptr, "low", 8, caps, 3);
        TAP_CHECK(hi >= 0 and lo >= 0); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_go); // reclaim: the suite must be pool-honest (runs on MAX_SEMAPHORES=4)
        TAP_CHECK(log_eq("lHL"));
    }

    // --- Core clock read syscall -----------------------------------------------
    void t_cpu_clock_hz()
    {
        uint32_t hz = kos_cpu_clock_hz();
        TAP_CHECK(hz == kos_cpu_clock_hz()); // read-only + stable across reads
        // 0 == the backend has no silicon core clock (host sim); a real core
        // reports a plausible rate (>= 1 MHz, below every board's post-init clock).
        TAP_CHECK(hz == 0u or hz >= 1000000u);
    }

    // Clock-select seam (M3): kos_cpu_clock_set is PRIVILEGED (syscall gate returns
    // the sentinel 0 == "cannot change" to any unprivileged caller, with NO retune).
    // This test exercises exactly that unprivileged-reject contract. It MUST run from
    // a spawned UNPRIVILEGED child: the selftest root thread is privileged (kmain), so
    // a call made here would actually retune on a chip with a real backend (XMC/K64F)
    // and leave the core clock moved for the rest of the suite. The privileged
    // real-retune + coherence tail (re-anchor / baud / re-arm) is covered by the
    // clockretune harness, silicon-only; see docs/design-m3-clock-select.md sec 6.
    uint32_t g_clkset_low = 1; // child: kos_cpu_clock_set(LOW), expect 0 (rejected)
    uint32_t g_clkset_mid = 1;
    uint32_t g_clkset_max = 1;
    int g_clkset_done = -1;
    void clkset_unpriv_worker(void*) // UNPRIVILEGED; caps: g_clkset_done@1 (CH_DONE)
    {
        g_clkset_low = kos_cpu_clock_set(KOS_PSTATE_LOW);
        g_clkset_mid = kos_cpu_clock_set(KOS_PSTATE_MID);
        g_clkset_max = kos_cpu_clock_set(KOS_PSTATE_MAX);
        kos_sem_post(CH_DONE); // g_clkset_done (delegated from main)
    }
    void t_cpu_clock_set()
    {
        uint32_t const before = kos_cpu_clock_hz();
        uint64_t const t0 = kos_clock_now();
        g_clkset_low = 1;
        g_clkset_mid = 1;
        g_clkset_max = 1;
        g_clkset_done = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_clkset_done, CH_FULL}}; // g_clkset_done@1 (CH_DONE)
        int w = kos::thread::spawn_caps(clkset_unpriv_worker, nullptr, "clkset", 10, caps, 1);
        if (w < 0)
        {
            kos::print("# cpu_clock_set: SKIP (thread pool too small)\n");
            kos_sem_destroy(g_clkset_done);
            return;
        }
        kos_sem_wait(g_clkset_done);
        kos_sem_destroy(g_clkset_done);
        // Unprivileged: the gate refuses every P-state -> 0, so no real retune fired.
        TAP_CHECK(g_clkset_low == 0u);
        TAP_CHECK(g_clkset_mid == 0u);
        TAP_CHECK(g_clkset_max == 0u);
        // The rejected seam left the clock and the monotonic time base untouched.
        TAP_CHECK(kos_cpu_clock_hz() == before);
        TAP_CHECK(kos_clock_now() >= t0);
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
        kos_sem_wait(CH_AUX); // g_irq
        log_put('W');
        kos_sem_post(CH_DONE);
    }
    void irq_injector(void*)
    {
        log_put('i');
        kos_irq_inject(5); // ISR posts g_irq -> higher-prio waiter preempts
        log_put('r');
        kos_sem_post(CH_DONE);
    }
    void t_irq()
    {
        log_reset();
        g_irq = kos_sem_create(0);
        kos_irq_attach(5, g_irq); // attach resolves MAIN's cap (needs CAP_SIGNAL), stores global
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_irq, CH_FULL}};
        kos_cap_grant icaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int w = kos::thread::spawn_caps(irq_waiter, nullptr, "irqW", 15, wcaps, 3);
        int inj = kos::thread::spawn_caps(irq_injector, nullptr, "irqI", 8, icaps, 2);
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
        kos_sem_post(CH_DONE);
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // -> done@1, lock@2
        int a = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                                        caps, 2, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                        /*privileged=*/true);
        int b = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                                        caps, 2, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                        /*privileged=*/true);
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
        kos_sem_post(CH_DONE);
    }
    void t_sleep()
    {
        log_reset();
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // -> done@1, lock@2
        int l = kos::thread::spawn_caps(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL",
                                        10, caps, 2);
        int s = kos::thread::spawn_caps(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS",
                                        10, caps, 2);
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
        kos_sem_wait(CH_AUX); // g_multi
        log_put(arg_char(arg));
        kos_sem_post(CH_DONE);
    }
    void t_multi()
    {
        log_reset();
        g_multi = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_multi, CH_FULL}};
        int a = kos::thread::spawn_caps(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10,
                                        caps, 3);
        int b = kos::thread::spawn_caps(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10,
                                        caps, 3);
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
        kos_sem_post(CH_READY); // g_irqdrv_ready: registered + about to park: safe to fire
        for (int i = 0; i < 3; i++)
        {
            irq.wait();                                      // parks in thread ctx
            g_seen[i] = *static_cast<volatile int*>(g_mmio); // read granted MMIO
            irq.ack();                                       // unmask
            kos_sem_post(CH_DONE);                           // g_irqdrv_done
        }
    }
    void t_irqdrv()
    {
        // Alloc before the sems: an alloc-fail early return must not leak them (pool-honest suite).
        g_mmio = kos_ram_alloc(4096);
        if (g_mmio == nullptr)
        {
            // A tiny RAM arena (microbit: 16 KiB SRAM) cannot spare a 4 KiB page for the
            // mock MMIO region: skip (still ok), like the pool-too-small skips below.
            kos::print("# irq_as_event: SKIP (4 KiB MMIO-page alloc failed -- board too small)\n");
            return;
        }
        *static_cast<volatile int*>(g_mmio) = 0;
        g_irqdrv_done = kos_sem_create(0);
        g_irqdrv_ready = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_irqdrv_done, CH_FULL}, {g_irqdrv_ready, CH_FULL}}; // done@1, ready@2
        int drv = kos::thread::spawn_caps(irq_driver, nullptr, "irqdrv", 15, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, g_mmio, 4096);
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
        kos_sem_post(CH_READY); // g_mask_ready
        for (int i = 0; i < 2; i++)
        {
            irq.wait();
            g_mask_serviced++;
            irq.ack();
            kos_sem_post(CH_DONE);
        }
    }
    void t_irq_mask()
    {
        g_mask_ready = kos_sem_create(0);
        g_mask_serviced = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_mask_ready, CH_FULL}}; // done@1, ready@2
        int drv = kos::thread::spawn_caps(mask_driver, nullptr, "maskdrv", 1, caps, 2); // below root
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
        kos_sem_post(CH_READY); // g_autorearm_ready
        for (int i = 0; i < 3; i++)
        {
            irq.wait(); // no ack: the next wait re-arms the line on its own
            g_autorearm_seen++;
            kos_sem_post(CH_DONE);
        }
    }
    void t_irq_autorearm()
    {
        g_autorearm_ready = kos_sem_create(0);
        g_autorearm_seen = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_autorearm_ready, CH_FULL}}; // done@1, ready@2
        int drv = kos::thread::spawn_caps(autorearm_driver, nullptr, "autoirq", 15, caps, 2);
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
        kos_sem_post(CH_READY); // g_phantom_ready
        irq.wait();            // consume fire #1 -> needs_rearm set, line masked
        irq.ack();             // ack;compute;wait shape: unmask now, needs_rearm clear
        kos_sem_post(CH_DONE); // acked; root injects the one mid-compute event
        irq.wait();            // consume that one event
        g_phantom_seen++;
        kos_sem_post(CH_DONE);
        irq.wait();            // MUST block: only one event was injected, no phantom
        g_phantom_seen++;      // reached only on a phantom wake (the bug)
        kos_sem_post(CH_DONE);
    }
    void t_irq_phantom()
    {
        g_phantom_ready = kos_sem_create(0);
        g_phantom_seen = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_phantom_ready, CH_FULL}}; // done@1, ready@2
        int drv = kos::thread::spawn_caps(phantom_driver, nullptr, "phantirq", 1, caps, 2); // below root
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
        // Malformed / out-of-range caps at the resolve boundary must fail-safe: negative,
        // garbage-huge, and an out-of-range index all reject. Via handle_close (the one
        // cap syscall that returns a value; wait/post share the same cap_resolve
        // chokepoint). Pool-neutral.
        TAP_CHECK(kos_handle_close(-1) == -1);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -1);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -1);
    }

    // --- Refcounted close of a DELEGATED sem: object survives while a co-holder is
    // parked; the last close frees it. (Replaces the old quiescent-only destroy: under
    // per-task caps, closing MY cap never destroys an object another task still holds.)
    int g_dsem = -1;
    void destroy_waiter(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        kos_sem_wait(CH_READY); // g_dsem: parks (initial 0)
        kos_sem_post(CH_DONE);
    }
    void destroy_poster(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        // Sleep past MAIN's close below, THEN post -- so the wake of the parked waiter
        // happens strictly AFTER MAIN has dropped its own (shared) cap on g_dsem.
        kos_sleep_ns(10000000ull);
        kos_sem_post(CH_READY); // wakes destroy_waiter
        kos_sem_post(CH_DONE);
    }
    void t_sem_destroy_busy()
    {
        g_dsem = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_dsem, CH_FULL}}; // done@1, dsem@2
        int w = kos::thread::spawn_caps(destroy_waiter, nullptr, "dwaiter", 15, caps, 2);
        int p = kos::thread::spawn_caps(destroy_poster, nullptr, "dposter", 15, caps, 2);
        TAP_CHECK(w >= 0 and p >= 0); // spawn failure would hang wait_n(2) below
        kos_sleep_ns(2000000ull);     // let the waiter park on g_dsem; refs = main+waiter+poster = 3
        // Close MAIN's cap WHILE the waiter is parked and the poster has not yet posted:
        // refs 3->2, so the object MUST survive (co-holders still name it) -- the headline
        // destroy-on-last-close semantics in its load-bearing case. If this freed or
        // corrupted the object or its wait queue, the poster's later post would not wake
        // the parked waiter and wait_n(2) would hang.
        TAP_CHECK(kos_handle_close(g_dsem) == 0);
        wait_n(2); // poster woke (post-sleep), posted g_dsem waking the parked waiter; both
                   // reported => object + wait queue intact after MAIN closed a shared cap.
        // Both holders have now exited (teardown closed their caps): refs -> 0, freed. Pool
        // honesty: create/close well past the pool size must never exhaust -> last close
        // reclaimed the slot. (t_sem_raii proves the general reclaim path; this is targeted.)
        for (int i = 0; i < 100; i++)
        {
            int s = kos_sem_create(0);
            TAP_CHECK(s >= 0 and kos_handle_close(s) == 0);
        }
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

    // --- PI mutex: basic lock/unlock + mutual exclusion (H1) -------------------
    // Three equal-priority workers each do ITERS non-atomic read-yield-write cycles
    // under the mutex. The kos_yield() inside the critical section hands the CPU to a
    // peer mid-update; if the lock did NOT serialize, the peer would read the stale
    // value and updates would be lost (final < expected). Exact conservation proves
    // mutual exclusion.
    constexpr int MTX_ITERS = 20;
    int g_mtx_shared = 0;
    // A mutex cap carries CAP_TRANSFER only (possession IS the lock/unlock authority,
    // no WAIT/SIGNAL split), so it must be delegated with a TRANSFER-only mask -- a
    // CH_FULL mask is not a subset and delegation would reject it.
    constexpr uint8_t CH_MTX = KOS_CAP_TRANSFER;
    void mtx_basic_worker(void*) // caps: done@1, mutex@2
    {
        for (int i = 0; i < MTX_ITERS; i++)
        {
            kos_mutex_lock(2);      // the delegated mutex cap
            int tmp = g_mtx_shared; // read
            kos_yield();            // yield MID critical section -> peer must not enter
            g_mtx_shared = tmp + 1; // write-back (lost if the lock didn't hold)
            kos_mutex_unlock(2);
        }
        kos_sem_post(CH_DONE);
    }
    void t_mutex_basic()
    {
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0);
        g_mtx_shared = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2
        int a = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbA", 10, caps, 2);
        int b = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbB", 10, caps, 2);
        int c = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbC", 10, caps, 2);
        if (a < 0 or b < 0 or c < 0)
        {
            // Tiny thread pool (microbit MAX_THREADS=2) can't host 3 workers. Drain the
            // ones that DID spawn (they post the shared g_done) so no stale post desyncs a
            // later wait_n, close the mutex so nothing leaks (stops the cap-table cascade),
            // then skip -- boards with a big enough pool run the full test.
            int n = 0;
            if (a >= 0) { n++; }
            if (b >= 0) { n++; }
            if (c >= 0) { n++; }
            wait_n(n);
            kos_handle_close(m);
            kos::print("# mutex_basic: SKIP (pool too small)\n");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(g_mtx_shared == 3 * MTX_ITERS); // no lost update -> mutual exclusion held
    }

    // Clock-scaled time unit (mirrors t_rr): measure one clock granule, then pick a
    // unit several granules wide so sleeps and busy-spins are resolvable on coarse
    // clocks (QEMU semihosting) as well as the fine sim clock.
    // Size the unit from the MEASURED reschedule cost, not the clock granule: the PI
    // choreography holds only if the lock/block/boost chain forms within the slack
    // between scheduled wakes, and that slack must dominate a reschedule round-trip --
    // which on a slow core (armv6m M0+: software 64-bit divides in the tickless math) is
    // far larger than the clock resolution the old 1 ms constant keyed on (the M0+
    // soft-failed the chain test at 1 ms; ~10-30 ms is enough). Floored at 1 ms so no
    // faster board that passed shrinks; capped so a pathological reading cannot stretch
    // the run.
    uint64_t mtx_time_unit()
    {
        // Clock resolution: a lower bound (a unit below a few granules is unmeasurable).
        uint64_t g0 = kos_clock_now();
        uint64_t g1 = g0;
        while (g1 == g0) { g1 = kos_clock_now(); }
        uint64_t g2 = g1;
        while (g2 == g1) { g2 = kos_clock_now(); }
        uint64_t granule = g2 - g1;

        // Reschedule cost: per-sleep OVERHEAD above a small real sleep (arm + idle +
        // wake + switch) -- the scheduling jitter a 1-unit gap must out-scale.
        constexpr uint32_t N = 8;
        constexpr uint64_t probe = 200000ull; // 200 us
        uint64_t t0 = kos_clock_now();
        for (uint32_t i = 0; i < N; i++)
        {
            kos_sleep_ns(probe);
        }
        uint64_t rt = (kos_clock_now() - t0) / N;
        uint64_t overhead = 0;
        if (rt > probe)
        {
            overhead = rt - probe;
        }

        uint64_t unit = overhead * 32;
        uint64_t const gfloor = granule * 4;
        if (unit < gfloor)
        {
            unit = gfloor;
        }
        if (unit < 1000000ull)
        {
            unit = 1000000ull; // 1 ms floor: fast-core behavior unchanged
        }
        if (unit > 30000000ull)
        {
            unit = 30000000ull; // 30 ms cap: glitch guard
        }
        return unit;
    }
    void mtx_spin(uint64_t ns)
    {
        uint64_t start = kos_clock_now();
        while (kos_clock_now() - start < ns)
        {
        }
    }

    // --- PI donation: boost-on-contention + revert-by-recompute (H2, H4, H8) ----
    // low(8) holds the mutex and busy-spins. high(20) wakes mid-spin and blocks on
    // the mutex, boosting low to 20. med(12) then wakes but must NOT preempt the
    // boosted low. So low finishes its critical section ('u') BEFORE med runs ('m')
    // -- the observable inversion-avoidance. After low unlocks it reverts to base 8,
    // so med (12) runs before low resumes ('z') -- the observable revert. high runs
    // the instant low hands off ('H' right after 'u').
    uint64_t g_mtx_unit = 1000000ull;
    void pi_low(void*) // caps: done@1, lock@2, mutex@3
    {
        kos_mutex_lock(3);
        log_put('l');
        mtx_spin(g_mtx_unit * 4); // hold across high's and med's wake instants
        log_put('u');
        kos_mutex_unlock(3); // hands off to high (preempts here); low reverts to base
        log_put('z');        // reached only after med (12) has run -> proves revert
        kos_sem_post(CH_DONE);
    }
    void pi_high(void*) // caps: done@1, lock@2, mutex@3
    {
        kos_sleep_ns(g_mtx_unit * 1);
        log_put('h');
        kos_mutex_lock(3); // low holds it -> block + boost low to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void pi_med(void*) // caps: done@1, lock@2
    {
        kos_sleep_ns(g_mtx_unit * 2);
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_pi()
    {
        log_reset();
        g_mtx_unit = mtx_time_unit();
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0);
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int lo = kos::thread::spawn_caps(pi_low, nullptr, "piLo", 8, lcaps, 3);
        int hi = kos::thread::spawn_caps(pi_high, nullptr, "piHi", 20, lcaps, 3);
        int md = kos::thread::spawn_caps(pi_med, nullptr, "piMd", 12, mcaps, 2);
        if (lo < 0 or hi < 0 or md < 0)
        {
            // microbit MAX_THREADS=2 can't host 3 workers: drain the spawned ones (they
            // post the shared g_done), close the mutex (no leak -> no cap-table cascade), skip.
            int n = 0;
            if (lo >= 0) { n++; }
            if (hi >= 0) { n++; }
            if (md >= 0) { n++; }
            wait_n(n);
            kos_handle_close(m);
            kos::print("# mutex_pi_donation: SKIP (pool too small)\n");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(count('l') == 1 and count('u') == 1 and count('h') == 1
                  and count('H') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('h', 1) < nth('u', 1)); // high contended while low still held it
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST: boosted low finished CS before med
        TAP_CHECK(nth('u', 1) < nth('H', 1)); // high acquired only after low released
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: low back at base, med ran first
    }

    // --- Chained/nested boost across two mutexes (H5) ---------------------------
    // A(20) waits on M1 owned by B(10); B waits on M2 owned by C(5). The boost must
    // PROPAGATE two hops: C is raised to A's priority. A medium thread D(15) wakes
    // while C spins; if the chain boost reached >= 15, D cannot preempt C, so C
    // finishes its critical section ('e') BEFORE D runs ('d'). That single ordering
    // is the whole chain: it can only hold if the boost travelled B -> C.
    void ch_c(void*) // caps: done@1, lock@2, M2@3
    {
        kos_mutex_lock(3); // M2
        log_put('c');
        mtx_spin(g_mtx_unit * 8); // hold past D's wake at 4u, with margin
        log_put('e');
        kos_mutex_unlock(3);
        log_put('C');
        kos_sem_post(CH_DONE);
    }
    void ch_b(void*) // caps: done@1, lock@2, M1@3, M2@4
    {
        kos_sleep_ns(g_mtx_unit * 1);
        kos_mutex_lock(3); // M1 (before A tries it)
        log_put('b');
        kos_mutex_lock(4); // M2: C holds it -> block, boost C to 10
        kos_mutex_unlock(4);
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_a(void*) // caps: done@1, lock@2, M1@3
    {
        kos_sleep_ns(g_mtx_unit * 2);
        kos_mutex_lock(3); // M1: B holds it -> block, boost B to 20, chain-boost C to 20
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_d(void*) // caps: done@1, lock@2
    {
        kos_sleep_ns(g_mtx_unit * 4); // wake well after the chain has fully formed (~2u)
        log_put('d');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_chain()
    {
        log_reset();
        g_mtx_unit = mtx_time_unit();
        int m1 = kos_mutex_create();
        int m2 = kos_mutex_create();
        TAP_CHECK(m1 >= 0 and m2 >= 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m2, CH_MTX}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}};
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int c = kos::thread::spawn_caps(ch_c, nullptr, "chC", 5, ccaps, 3);
        int b = kos::thread::spawn_caps(ch_b, nullptr, "chB", 10, bcaps, 4);
        int a = kos::thread::spawn_caps(ch_a, nullptr, "chA", 20, acaps, 3);
        int d = kos::thread::spawn_caps(ch_d, nullptr, "chD", 15, dcaps, 2);
        if (c < 0 or b < 0 or a < 0 or d < 0)
        {
            // microbit MAX_THREADS=2 can't host 4 workers: drain the spawned ones (they
            // post the shared g_done), close both mutexes (no leak -> no cascade), skip.
            int n = 0;
            if (c >= 0) { n++; }
            if (b >= 0) { n++; }
            if (a >= 0) { n++; }
            if (d >= 0) { n++; }
            wait_n(n);
            kos_handle_close(m1);
            kos_handle_close(m2);
            kos::print("# mutex_chain_boost: SKIP (pool too small)\n");
            return;
        }
        wait_n(4);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        TAP_CHECK(count('c') == 1 and count('e') == 1 and count('d') == 1
                  and count('b') == 1 and count('C') == 1);
        TAP_CHECK(nth('b', 1) < nth('e', 1)); // chain formed (B took M1 before C released M2)
        TAP_CHECK(nth('e', 1) < nth('d', 1)); // CHAIN BOOST: C ran above med across two hops
    }

    // --- Owner dies holding the mutex: waiter gets OWNER_DIED (H7, R3) ----------
    // Sleep-sequenced (like the PI test) so it does not depend on privileged-main's
    // posts preempting synchronously: owner (low) acquires and holds across a sleep,
    // the higher-priority waiter wakes mid-hold and blocks on the mutex, then the
    // owner wakes and exits WHILE still holding -> cap_teardown force-unlocks and the
    // woken waiter's lock() returns OWNER_DIED.
    int g_od_result = -99;
    void od_owner(void*) // caps: mutex@1, holds@2
    {
        kos_mutex_lock(1);
        kos_sem_post(2);              // holds: owner now owns it
        kos_sleep_ns(g_mtx_unit * 3); // hold past the waiter's block, then exit owning
        kos_exit(0);                  // exits still owning -> force-unlock (R3)
    }
    void od_waiter(void*) // caps: done@1, mutex@2
    {
        kos_sleep_ns(g_mtx_unit * 1);    // wake while the owner still holds it
        g_od_result = kos_mutex_lock(2); // block; woken by the dying owner with OWNER_DIED
        if (g_od_result >= 0)
        {
            kos_mutex_unlock(2);
        }
        kos_sem_post(CH_DONE);
    }
    void t_mutex_owner_died()
    {
        g_od_result = -99;
        g_mtx_unit = mtx_time_unit();
        int m = kos_mutex_create();
        int holds = kos_sem_create(0);
        TAP_CHECK(m >= 0 and holds >= 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}}; // mtx@1, holds@2
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mtx@2
        int ow = kos::thread::spawn_caps(od_owner, nullptr, "odOwn", 8, ocaps, 2);
        int wt = kos::thread::spawn_caps(od_waiter, nullptr, "odWt", 12, wcaps, 2);
        TAP_CHECK(ow >= 0 and wt >= 0);
        kos_sem_wait(holds); // owner acquired the mutex (then sleeps, still holding)
        wait_n(1);           // only the waiter posts done (owner exited)
        TAP_CHECK(g_od_result == KOS_MUTEX_OWNER_DIED);
        TAP_CHECK(kos_handle_close(m) == 0);
        kos_sem_destroy(holds);
    }

    // --- Deadlock refused with -2 (H6): self-lock + a two-mutex wait cycle ------
    int g_cyc_rb = -99;
    void cyc_a(void*) // caps: done@1, M1@2, M2@3, have1@4, goA@5
    {
        kos_mutex_lock(2); // M1
        kos_sem_post(4);   // have1
        kos_sem_wait(5);   // goA
        int r = kos_mutex_lock(3); // M2: B holds -> block; later handed off (r==0)
        if (r == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2);
        kos_sem_post(CH_DONE);
    }
    void cyc_b(void*) // caps: done@1, M2@2, M1@3, have2@4, goB@5
    {
        kos_mutex_lock(2); // M2
        kos_sem_post(4);   // have2
        kos_sem_wait(5);   // goB
        g_cyc_rb = kos_mutex_lock(3); // M1: closes the cycle -> refused with -2
        if (g_cyc_rb == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2); // release M2 -> hands it to A
        kos_sem_post(CH_DONE);
    }
    void t_mutex_deadlock()
    {
        // Self-deadlock: a recursive lock is refused (-2), not parked, and leaves the
        // mutex holdable/releasable normally.
        int self = kos_mutex_create();
        TAP_CHECK(self >= 0);
        TAP_CHECK(kos_mutex_lock(self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == -2); // recursive -> refused
        TAP_CHECK(kos_mutex_unlock(self) == 0);
        TAP_CHECK(kos_handle_close(self) == 0);

        // Cross-thread cycle: A owns M1 + waits M2; B owns M2 + tries M1 -> -2.
        g_cyc_rb = -99;
        int m1 = kos_mutex_create();
        int m2 = kos_mutex_create();
        int have1 = kos_sem_create(0);
        int have2 = kos_sem_create(0);
        int goA = kos_sem_create(0);
        int goB = kos_sem_create(0);
        if (m1 < 0 or m2 < 0 or have1 < 0 or have2 < 0 or goA < 0 or goB < 0)
        {
            // The cross-thread cycle needs 2 mutexes + 4 sems live at once; microbit's
            // cap table (MAX_HANDLES=6, 3 free) / sem pool (MAX_SEMAPHORES=4, 2 free) can't
            // hold them. No worker has spawned yet, so just reclaim what was created (in
            // any order -- close/destroy ignores a <0 handle) and skip.
            if (m1 >= 0) { kos_handle_close(m1); }
            if (m2 >= 0) { kos_handle_close(m2); }
            if (have1 >= 0) { kos_sem_destroy(have1); }
            if (have2 >= 0) { kos_sem_destroy(have2); }
            if (goA >= 0) { kos_sem_destroy(goA); }
            if (goB >= 0) { kos_sem_destroy(goB); }
            kos::print("# mutex_deadlock: SKIP (pool too small)\n");
            return;
        }
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {m1, CH_MTX}, {m2, CH_MTX},
                                 {have1, CH_FULL}, {goA, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {m2, CH_MTX}, {m1, CH_MTX},
                                 {have2, CH_FULL}, {goB, CH_FULL}};
        int a = kos::thread::spawn_caps(cyc_a, nullptr, "cycA", 10, acaps, 5);
        int b = kos::thread::spawn_caps(cyc_b, nullptr, "cycB", 10, bcaps, 5);
        TAP_CHECK(a >= 0 and b >= 0);
        kos_sem_wait(have1); // A owns M1
        kos_sem_wait(have2); // B owns M2
        kos_sem_post(goA);   // A tries M2 -> blocks (B owns it)
        kos_sem_post(goB);   // B tries M1 -> would cycle -> -2, not parked
        wait_n(2);
        TAP_CHECK(g_cyc_rb == -2);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        kos_sem_destroy(have1);
        kos_sem_destroy(have2);
        kos_sem_destroy(goA);
        kos_sem_destroy(goB);
    }

    // --- Closing a mutex you OWN is refused (R2) --------------------------------
    void t_mutex_close_owned()
    {
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0);
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == -1); // refused: you cannot close a mutex you hold
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);  // released -> close now succeeds
    }

    // --- Multiple held mutexes: revert is recompute, NOT restore-to-base (H3) ---
    // B (base 6) holds M1 and M2; H (prio 20) waits on M1, boosting B to 20; D (12)
    // competes. B unlocks M2 while H still waits on M1: with recompute B STAYS at 20
    // (M1's waiter still floors it), so D cannot preempt B and B runs on to unlock M1
    // -> H acquires and runs BEFORE D. A restore-to-base bug would drop B to 6 at the
    // M2 unlock, letting D(12) preempt immediately -> D would run before H. So
    // nth('H') < nth('d') is the discriminator.
    void mh_b(void*) // caps: done@1, lock@2, M1@3, M2@4
    {
        kos_mutex_lock(3); // M1
        kos_mutex_lock(4); // M2
        log_put('b');
        mtx_spin(g_mtx_unit * 3); // hold across H's block on M1
        kos_mutex_unlock(4);      // release M2 while H waits on M1 -> B must STAY boosted
        log_put('x');
        mtx_spin(g_mtx_unit * 3); // hold across D's wake; boosted B must not be preempted
        kos_mutex_unlock(3);      // release M1 -> hand to H, B drops to base
        kos_sem_post(CH_DONE);
    }
    void mh_h(void*) // caps: done@1, lock@2, M1@3
    {
        kos_sleep_ns(g_mtx_unit * 1);
        kos_mutex_lock(3); // M1: B holds -> block, boost B to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void mh_d(void*) // caps: done@1, lock@2
    {
        kos_sleep_ns(g_mtx_unit * 4); // wake after B unlocked M2, while B should still be boosted
        log_put('d');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_multi_held()
    {
        log_reset();
        g_mtx_unit = mtx_time_unit();
        int m1 = kos_mutex_create();
        int m2 = kos_mutex_create();
        TAP_CHECK(m1 >= 0 and m2 >= 0);
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}};
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int b = kos::thread::spawn_caps(mh_b, nullptr, "mhB", 6, bcaps, 4);
        int h = kos::thread::spawn_caps(mh_h, nullptr, "mhH", 20, hcaps, 3);
        int d = kos::thread::spawn_caps(mh_d, nullptr, "mhD", 12, dcaps, 2);
        if (b < 0 or h < 0 or d < 0)
        {
            // microbit MAX_THREADS=2 can't host 3 workers: drain the spawned ones (they
            // post the shared g_done), close both mutexes (no leak -> no cascade), skip.
            int n = 0;
            if (b >= 0) { n++; }
            if (h >= 0) { n++; }
            if (d >= 0) { n++; }
            wait_n(n);
            kos_handle_close(m1);
            kos_handle_close(m2);
            kos::print("# mutex_multi_held: SKIP (pool too small)\n");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        TAP_CHECK(count('b') == 1 and count('x') == 1 and count('H') == 1 and count('d') == 1);
        TAP_CHECK(nth('x', 1) < nth('H', 1)); // M2 released before M1 handed off
        TAP_CHECK(nth('H', 1) < nth('d', 1)); // RECOMPUTE: B stayed boosted, H ran before D
    }

    // --- unlock by a non-owner / of an unlocked mutex both return -1 ------------
    // The runtime owner check that became user-reachable (the old KICKOS_ASSERT must
    // never panic once exposed).
    int g_nonowner_rc = -99;
    void nonowner_unlock(void*) // caps: done@1, mutex@2
    {
        g_nonowner_rc = kos_mutex_unlock(2); // caller is not the owner -> -1
        kos_sem_post(CH_DONE);
    }
    void t_mutex_unlock_errors()
    {
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0);
        TAP_CHECK(kos_mutex_unlock(m) == -1); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2
        int w = kos::thread::spawn_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_nonowner_rc == -1);       // non-owner unlock refused, no panic
        TAP_CHECK(kos_mutex_unlock(m) == 0);  // the real owner still unlocks
        TAP_CHECK(kos_handle_close(m) == 0);
    }

    // --- Owner dies holding with NO waiter: m->owner cleared, re-lockable (R3) ---
    void od_solo_owner(void*) // caps: mutex@1, holds@2
    {
        kos_mutex_lock(1);
        kos_sem_post(2); // holds
        kos_exit(0);     // exits owning, no waiter -> force-unlock nulls m->owner
    }
    void t_mutex_owner_died_nowaiter()
    {
        int m = kos_mutex_create();
        int holds = kos_sem_create(0);
        TAP_CHECK(m >= 0 and holds >= 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}}; // mtx@1, holds@2
        int ow = kos::thread::spawn_caps(od_solo_owner, nullptr, "odSolo", 8, ocaps, 2);
        TAP_CHECK(ow >= 0);
        kos_sem_wait(holds); // owner acquired, then exits (higher prio, runs to exit)
        // If force-unlock did not null m->owner, this lock would block forever on a
        // dead owner. It must acquire cleanly (fresh, uncontended -> 0).
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        kos_sem_destroy(holds);
    }

    // --- Delegated-mutex refcount: child closes its cap, parent still locks ------
    void deleg_closer(void*) // caps: done@1, mutex@2
    {
        kos_handle_close(2);   // drop the child's delegated cap (refs 2 -> 1)
        kos_sem_post(CH_DONE);
    }
    void t_mutex_deleg_refcount()
    {
        int m = kos_mutex_create(); // refs = 1 (main)
        TAP_CHECK(m >= 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2 (refs -> 2)
        int w = kos::thread::spawn_caps(deleg_closer, nullptr, "delcl", 10, caps, 2);
        TAP_CHECK(w >= 0);
        wait_n(1);
        // Child closed its cap (and exited): the object must survive on main's cap.
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0); // last close frees it
        // Pool honesty: create/close well past the pool must not exhaust.
        for (int i = 0; i < 40; i++)
        {
            int x = kos_mutex_create();
            TAP_CHECK(x >= 0 and kos_handle_close(x) == 0);
        }
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
    void caller_stack_worker(void*) { kos_sem_post(CH_DONE); } // g_cstk_sem; ran on the caller's stack
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
        kos_cap_grant caps[] = {{g_cstk_sem, CH_FULL}}; // -> g_cstk_sem @1 (CH_DONE)
        int const t = kos::thread::spawn(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, stk, kStk, nullptr, 0, caps, 1);
        TAP_CHECK(t >= 0);        // spawn accepted the caller-owned stack
        kos_sem_wait(g_cstk_sem); // the worker ran on it and posted
        kos_sem_destroy(g_cstk_sem);
#if !KICKOS_HAVE_MPU
        // Same shape via a statically-defined KOS_STACK_DEFINE buffer, unprivileged. This
        // buffer is only 16-byte aligned (no MPU); spawn must still accept + run it -- the
        // path that regressed, since with no region descriptor the natural-alignment check
        // must not apply.
        g_cstk_sem = kos_sem_create(0);
        kos_cap_grant scaps[] = {{g_cstk_sem, CH_FULL}}; // -> g_cstk_sem @1 (CH_DONE)
        int const ts = kos::thread::spawn(caller_stack_worker, nullptr, "cstkS", 10, KOS_POLICY_FIFO,
                                          0, false, nullptr, 0, g_cstk_static,
                                          static_cast<uint32_t>(sizeof(g_cstk_static)),
                                          nullptr, 0, scaps, 1);
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
    void dom_writer(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        *g_dshared = kDomSentinel; // write the shared domain region (granted)
        kos_sem_post(CH_DONE);     // g_dwrote
    }
    void dom_reader(void*) // caps: g_dwrote@1 (CH_DONE), g_dread@2 (CH_READY)
    {
        kos_sem_wait(CH_DONE);    // g_dwrote: after the writer stored the sentinel
        g_dreadback = *g_dshared; // read the SAME region -> proves the shared grant
        kos_sem_post(CH_READY);   // g_dread
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
        kos_cap_grant wcaps[] = {{g_dwrote, CH_FULL}};                    // g_dwrote@1
        kos_cap_grant rcaps[] = {{g_dwrote, CH_FULL}, {g_dread, CH_FULL}}; // g_dwrote@1, g_dread@2
        int w = kos::thread::spawn_caps(dom_writer, nullptr, "domW", 10, wcaps, 1, KOS_POLICY_FIFO,
                                        0, false, const_cast<int*>(g_dshared), 256);
        int r = kos::thread::spawn_caps(dom_reader, nullptr, "domR", 10, rcaps, 2, KOS_POLICY_FIFO,
                                        0, false, const_cast<int*>(g_dshared), 256);
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
        kos_sem_post(CH_DONE); // g_mmio_done
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
        kos_cap_grant caps[] = {{g_mmio_done, CH_FULL}}; // g_mmio_done@1 (CH_DONE)
        int w = kos::thread::spawn_caps(mmio_unpriv_worker, nullptr, "mmioW", 10, caps, 1);
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
    void cd_kid(void* arg) // caps: g_cd_kidsem@1 (CH_DONE), delegated by cd_worker
    {
        *static_cast<int*>(arg) = 1;
        kos_sem_post(CH_DONE); // g_cd_kidsem (this grandchild's delegated cap)
    }
    void cd_worker(void*) // UNPRIVILEGED; caps: g_cd_done@1 (CH_DONE), delegated by main
    {
        g_cd_lit_rc = kos_kconsole_write(kCdLit, strlen(kCdLit)); // rodata: accepted

        // cd_worker creates its OWN sem (unprivileged create is allowed) and RE-delegates
        // it to a grandchild -- nested delegation requires the source cap carry TRANSFER,
        // which sem_create grants. g_cd_kidsem is cd_worker's cap value (its table).
        g_cd_kidsem = kos_sem_create(0);
        kos_cap_grant kidcaps[] = {{g_cd_kidsem, CH_FULL}}; // -> grandchild's index 1
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        g_cd_goodspawn = kos::thread::spawn_caps(cd_kid, &g_cd_goodname_ran, "cdgood", 9,
                                                 kidcaps, 1);
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
            g_cd_badname_spawn = kos::thread::spawn_caps(cd_kid, &g_cd_badname_ran,
                                                         static_cast<char const*>(bad), 9,
                                                         kidcaps, 1);
            if (g_cd_badname_spawn >= 0)
            {
                kos_sem_wait(g_cd_kidsem);
            }
            g_cd_neg_ran = 1;
        }
#endif
        kos_sem_destroy(g_cd_kidsem); // close cd_worker's own cap
        kos_sem_post(CH_DONE);        // g_cd_done (delegated from main)
    }
    void t_confused_deputy()
    {
        g_cd_done = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_cd_done, CH_FULL}}; // g_cd_done@1 (CH_DONE)
        int w = kos::thread::spawn_caps(cd_worker, nullptr, "cdwork", 10, caps, 1);
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

    // --- Endpoint IPC: synchronous rendezvous send/recv (M3 #4 stage i) ----------
    // The endpoint cap is delegated to workers at child index 2 (done@1, E@2). Workers
    // are UNPRIVILEGED so the kernel's copy into/from a parked peer runs against real
    // enforcement (the cross-domain privileged write, design section 3.1).
    char const kEpMsg[] = "hello-endpoint"; // 14 bytes (strlen), no NUL sent
    constexpr uint8_t EP_SIGNAL_ONLY = KOS_CAP_SIGNAL; // send right only
    constexpr uint8_t EP_WAIT_ONLY = KOS_CAP_WAIT;     // recv right only
    int g_ep = -1; // main's endpoint cap (created per test)
    char g_ep_rbuf[64];
    volatile long g_ep_rn = -99;         // worker recv return
    volatile uint32_t g_ep_rbadge = 0xffffffffu;
    volatile int g_ep_rcap = 64;         // capacity the recv worker passes
    volatile long g_ep_sn = -99;         // worker send return

    void ep_recv_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        // The recv buffer is a STACK local (in the thread's own granted stack region):
        // an unprivileged caller's writable check has no text fallback, so a global here
        // would be rejected on the sim / no-MPU backends. Copy the result into a global
        // (a direct store, not a syscall) for main to inspect.
        char buf[64];
        uint32_t badge = 0xdeadu;
        long n = kos_recv(2, buf, static_cast<size_t>(g_ep_rcap), &badge);
        g_ep_rn = n;
        g_ep_rbadge = badge;
        size_t k = 0;
        if (n > 0)
        {
            k = static_cast<size_t>(n);
            if (k > sizeof(buf))
            {
                k = sizeof(buf);
            }
            memcpy(g_ep_rbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    void ep_send_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        g_ep_sn = kos_send(2, kEpMsg, strlen(kEpMsg));
        kos_sem_post(CH_DONE);
    }

    void t_endpoint_rendezvous()
    {
        size_t const mlen = strlen(kEpMsg);
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}}; // done@1, E@2

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rcap = 64;
        int w = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eprx", 12, caps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        kos_sleep_ns(3000000ull); // let the worker park in recv
        long sc = kos_send(g_ep, kEpMsg, mlen);
        TAP_CHECK(sc == static_cast<long>(mlen)); // sender delivered n bytes
        wait_n(1);
        TAP_CHECK(g_ep_rn == static_cast<long>(mlen) and memcmp(g_ep_rbuf, kEpMsg, mlen) == 0);
        TAP_CHECK(g_ep_rbadge == 0); // badge always written on success (stage i: 0)

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        int w2 = kos::thread::spawn_caps(ep_send_worker, nullptr, "eptx", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w2 >= 0);
        kos_sleep_ns(3000000ull); // let the worker park in send
        char rbuf[64];
        uint32_t badge = 0xdeadu;
        long rc = kos_recv(g_ep, rbuf, sizeof(rbuf), &badge);
        TAP_CHECK(rc == static_cast<long>(mlen) and memcmp(rbuf, kEpMsg, mlen) == 0);
        TAP_CHECK(badge == 0);
        wait_n(1);
        TAP_CHECK(g_ep_sn == static_cast<long>(mlen));

        // (C) zero-length is a valid signal (n == 0 on both sides, NOT -1).
        g_ep_rn = -99; g_ep_rcap = 64;
        int w3 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "epz", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w3 >= 0);
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, kEpMsg, 0) == 0);
        wait_n(1);
        TAP_CHECK(g_ep_rn == 0);

        // (D) truncation: send mlen into a 4-byte capacity -> both return 4.
        g_ep_rn = -99; g_ep_rcap = 4;
        int w4 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eptr", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w4 >= 0);
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, kEpMsg, mlen) == 4);
        wait_n(1);
        TAP_CHECK(g_ep_rn == 4 and memcmp(g_ep_rbuf, kEpMsg, 4) == 0);

        TAP_CHECK(kos_handle_close(g_ep) == 0); // last cap -> endpoint freed
    }

    // --- Oversize reject + bad cap (main only; no parking) -----------------------
    void t_endpoint_reject()
    {
        char big[KOS_EP_MSG_MAX + 8];
        memset(big, 'x', sizeof(big));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        // Oversize send is rejected up front (F4) -- returns -1 WITHOUT parking (main is
        // the sole WAIT holder, so a park would hang the suite).
        TAP_CHECK(kos_send(g_ep, big, KOS_EP_MSG_MAX + 1) == -1);
        // Bad caps reject at the resolve boundary on both paths.
        char one[1] = {0};
        TAP_CHECK(kos_send(0x7fffffff, one, 1) == -1);
        TAP_CHECK(kos_recv(0x7fffffff, g_ep_rbuf, 1, nullptr) == -1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Rights denial: send needs SIGNAL, recv needs WAIT -----------------------
    volatile int g_ep_wait_send_rc = -99;   // WAIT-only cap send -> -1
    volatile int g_ep_signal_recv_rc = -99; // SIGNAL-only cap recv -> -1
    void ep_rights_worker(void*) // caps: done@1, E(WAIT)@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        g_ep_wait_send_rc = static_cast<int>(kos_send(2, b, 1));   // WAIT-only -> no SIGNAL -> -1
        g_ep_signal_recv_rc = static_cast<int>(kos_recv(3, b, sizeof(b), nullptr)); // SIGNAL-only -> -1
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_rights()
    {
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        g_ep_wait_send_rc = -99; g_ep_signal_recv_rc = -99;
        // Two narrowed caps to the same endpoint: WAIT-only at index 2, SIGNAL-only at 3.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {g_ep, EP_SIGNAL_ONLY}};
        int w = kos::thread::spawn_caps(ep_rights_worker, nullptr, "eprt", 12, caps, 3,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_ep_wait_send_rc == -1);   // send refused without SIGNAL
        TAP_CHECK(g_ep_signal_recv_rc == -1); // recv refused without WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- EPIPE: a parked sender is woken -1 when the last WAIT-cap holder drops it -
    // A SIGNAL-only delegation does NOT bump recv_holders, so main's cap is the sole
    // WAIT holder: closing it takes recv_holders 1->0 and EPIPEs the parked sender.
    volatile long g_ep_epipe_rc = -99;
    void ep_epipe_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_ep_epipe_rc = kos_send(2, kEpMsg, strlen(kEpMsg)); // parks; woken -1 on EPIPE
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_epipe()
    {
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        g_ep_epipe_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        int w = kos::thread::spawn_caps(ep_epipe_worker, nullptr, "epep", 12, caps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        kos_sleep_ns(3000000ull);              // let the sender park (recv_holders == 1 == main)
        TAP_CHECK(kos_handle_close(g_ep) == 0); // last WAIT cap -> EPIPE the parked sender
        wait_n(1);
        TAP_CHECK(g_ep_epipe_rc == -1); // sender woken with EPIPE, not a byte count
    }

    // --- Dead endpoint (unparked): send after the last WAIT cap is gone -> -1 -----
    // Distinct from the parked-then-EPIPE case: the sender never parks (F1 dead-check).
    volatile long g_ep_dead_rc = -99;
    int g_ep_go = -1;
    void ep_dead_worker(void*) // caps: done@1, E(SIGNAL)@2, go@3
    {
        kos_sem_wait(3);                                     // go: main has dropped its WAIT cap
        g_ep_dead_rc = kos_send(2, kEpMsg, strlen(kEpMsg)); // recv_holders == 0 -> -1 immediately
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_dead()
    {
        g_ep = kos_endpoint_create();
        g_ep_go = kos_sem_create(0);
        TAP_CHECK(g_ep >= 0 and g_ep_go >= 0);
        g_ep_dead_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}, {g_ep_go, CH_FULL}};
        int w = kos::thread::spawn_caps(ep_dead_worker, nullptr, "epde", 12, caps, 3,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        // Close main's (only) WAIT cap FIRST: recv_holders -> 0, no sender parked yet.
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        kos_sem_post(g_ep_go); // now the worker sends into the dead endpoint
        wait_n(1);
        TAP_CHECK(g_ep_dead_rc == -1); // rejected immediately, never parked
        kos_sem_destroy(g_ep_go);
    }

#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    // --- Bound-check: a recv/send pointer outside the caller's regions -> -1 ------
    // The write-oracle / cross-domain-read is closed the same way as the console
    // buffer: an unprivileged caller cannot launder an un-owned page through IPC.
    volatile long g_ep_badrecv_rc = -99;
    volatile long g_ep_badsend_rc = -99;
    int g_ep_bnd_neg_ran = 0;
    void ep_bound_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        void* bad = kos_guard_addr(); // an arena page granted to no domain
        if (bad != nullptr)
        {
            g_ep_badrecv_rc = kos_recv(2, bad, 8, nullptr);           // write oracle -> -1
            g_ep_badsend_rc = kos_send(2, static_cast<char const*>(bad), 8); // cross-domain read -> -1
            g_ep_bnd_neg_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_bound()
    {
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        g_ep_badrecv_rc = -99; g_ep_badsend_rc = -99; g_ep_bnd_neg_ran = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}}; // done@1, E@2
        int w = kos::thread::spawn_caps(ep_bound_worker, nullptr, "epbn", 12, caps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        wait_n(1);
        if (g_ep_bnd_neg_ran)
        {
            TAP_CHECK(g_ep_badrecv_rc == -1); // bad recv buffer rejected, never parked
            TAP_CHECK(g_ep_badsend_rc == -1); // bad send buffer rejected, never parked
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }
#endif

    // --- Cross-domain rendezvous under enforcement (F5) --------------------------
    // Two UNPRIVILEGED workers in DIFFERENT memory domains rendezvous: the arriving
    // side's kernel copy lands in the parked peer's domain (not the arriver's loaded
    // regions), exercising the privileged background write. The payload buffers live
    // in each worker's own granted domain region. Delegation accounting is validated
    // by the clean endpoint free at the end (both a WAIT and a SIGNAL cap delegated).
    volatile long g_xd_send_rc = -99;
    volatile long g_xd_recv_rc = -99;
    volatile int g_xd_match = 0;
    int g_xd_done = -1; // PRIVATE completion sem: workers post it at CH_DONE, not the shared g_done
    void xd_send_worker(void* arg) // caps: done@1, E(SIGNAL)@2; arg = domain buffer
    {
        char* b = static_cast<char*>(arg);
        for (size_t i = 0; i < 8; i++)
        {
            b[i] = static_cast<char>('a' + i);
        }
        g_xd_send_rc = kos_send(2, b, 8);
        kos_sem_post(CH_DONE);
    }
    void xd_recv_worker(void* arg) // caps: done@1, E(WAIT)@2; arg = domain buffer
    {
        char* b = static_cast<char*>(arg);
        long n = kos_recv(2, b, 8, nullptr);
        g_xd_recv_rc = n;
        int ok = 1;
        for (int i = 0; i < 8; i++)
        {
            if (b[i] != static_cast<char>('a' + i))
            {
                ok = 0;
            }
        }
        g_xd_match = ok;
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_crossdomain()
    {
        void* sbuf = kos_ram_alloc(256);
        void* rbuf = kos_ram_alloc(256);
        if (sbuf == nullptr or rbuf == nullptr)
        {
            kos::print("# endpoint_crossdomain: SKIP (arena cannot spare two domain regions)\n");
            return;
        }
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        g_xd_done = kos_sem_create(0); // PRIVATE: never satisfies another test's wait_n(g_done)
        g_xd_send_rc = -99; g_xd_recv_rc = -99; g_xd_match = 0;
        kos_cap_grant scaps[] = {{g_xd_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        kos_cap_grant rcaps[] = {{g_xd_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};   // done@1, E(WAIT)@2
        int s = kos::thread::spawn_caps(xd_send_worker, sbuf, "xdTx", 12, scaps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false, sbuf, 256);
        int r = kos::thread::spawn_caps(xd_recv_worker, rbuf, "xdRx", 12, rcaps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false, rbuf, 256);
        if (s < 0 or r < 0)
        {
            kos::print("# endpoint_crossdomain: SKIP (thread pool too small for 2 concurrent)\n");
            // A lone sender parks then EPIPE-wakes on the close below and posts g_xd_done; a lone
            // receiver is an accepted permanent park (design 4.1: no receiver-side EPIPE) and is
            // NOT swept up. Either way the post lands on this test's PRIVATE sem, so it cannot
            // falsely satisfy the next test; we drop main's caps and do not wait for completion.
            kos_handle_close(g_ep);
            kos_sem_destroy(g_xd_done);
            return;
        }
        for (int i = 0; i < 2; i++)
        {
            kos_sem_wait(g_xd_done); // this test's own completion sem, not the shared g_done
        }
        TAP_CHECK(g_xd_send_rc == 8 and g_xd_recv_rc == 8);
        TAP_CHECK(g_xd_match == 1); // the byte-exact payload crossed domains
        TAP_CHECK(kos_handle_close(g_ep) == 0); // both delegated caps already torn down -> freed
        kos_sem_destroy(g_xd_done);
    }

    // --- B3: index 0 is the kernel stdout slot; an own create never lands there ---------
    void t_cap_index0()
    {
        // The low KCAP_INDEX_BITS bits of a cap handle are its table slot (cap.h:
        // KCAP_INDEX_BITS == 4). cap_install scans from 1, so an own sem/endpoint/mutex
        // create never returns slot 0 -- that slot is filled only by the console default.
        constexpr int kIdxMask = 0xF;
        int s = kos_sem_create(0);
        TAP_CHECK(s >= 0 and (s & kIdxMask) != 0);
        int e = kos_endpoint_create();
        TAP_CHECK(e >= 0 and (e & kIdxMask) != 0);
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0 and (m & kIdxMask) != 0);
        TAP_CHECK(kos_handle_close(s) == 0);
        TAP_CHECK(kos_handle_close(e) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);

        // Pre-publish (the sim never hands over -- a real publish would silence this TAP
        // stream), g_stdout_target < 0, so cap_install_defaults seats NOTHING at index 0.
        // Sending on the empty stdout slot therefore fails cleanly rather than resolving a
        // stale/aliased object -- this exercises the pre-publish cap_install_defaults branch.
        TAP_CHECK(kos_send(0, "x", 1) == -1);

        // Exhaustion: own-creates fill the remaining slots [1 .. MAX_HANDLES-1] and then
        // fail cleanly with -1 -- slot 0 stays reserved even at the LAST free slot, and a
        // full table never crashes or returns 0. (Index field is 4 bits: MAX_HANDLES <= 16.)
        int held[16];
        int n = 0;
        for (;;)
        {
            int h = kos_sem_create(0);
            if (h < 0)
            {
                break;
            }
            TAP_CHECK((h & kIdxMask) != 0); // never slot 0, not even the last free one
            held[n] = h;
            n = n + 1;
            if (n >= static_cast<int>(sizeof(held) / sizeof(held[0])))
            {
                break;
            }
        }
        TAP_CHECK(n >= 1);
        TAP_CHECK(kos_sem_create(0) == -1); // table full -> clean -1
        TAP_CHECK(kos_sem_create(0) == -1); // still -1 (idempotent failure, no side effect)
        for (int i = 0; i < n; i++)
        {
            TAP_CHECK(kos_handle_close(held[i]) == 0);
        }
        int again = kos_sem_create(0); // table recovers once slots are freed
        TAP_CHECK(again >= 0 and (again & kIdxMask) != 0);
        TAP_CHECK(kos_handle_close(again) == 0);
    }

    // --- console_publish is privileged-only; a bad cap is rejected with no side effect --
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console -- the rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1);
        kos_sem_post(CH_DONE);
    }
    void t_console_publish()
    {
        // Privileged MAIN: a bad/stale cap is rejected before the deinit/flip, so the
        // console stays kernel-owned (a real publish here would silence the TAP output).
        TAP_CHECK(kos_console_publish(-1) == -1);
        TAP_CHECK(kos_console_publish(0x7fffffff) == -1);
        // Unprivileged child: the privileged-only gate rejects it.
        g_pub_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int w = kos::thread::spawn_caps(pub_denied_worker, nullptr, "pubDen", 10, caps, 1);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_pub_rc == -1);
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
    tap::add("cpu_clock_hz", t_cpu_clock_hz);
    tap::add("cpu_clock_set", t_cpu_clock_set);
    tap::add("rr_interleave", t_rr);
    tap::add("sleep_order", t_sleep);
    tap::add("multi_wait", t_multi);
    tap::add("sem_destroy", t_sem_destroy);
    tap::add("sem_destroy_quiescent", t_sem_destroy_busy);
    tap::add("sem_raii", t_sem_raii);
    // PI-mutex capability (M3): production syscalls only, so runs on every board.
    tap::add("mutex_basic", t_mutex_basic);             // H1 mutual exclusion
    tap::add("mutex_pi_donation", t_mutex_pi);          // H2/H4/H8 boost + revert
    tap::add("mutex_chain_boost", t_mutex_chain);       // H5 chained boost
    tap::add("mutex_owner_died", t_mutex_owner_died);   // H7/R3 exit-while-owning
    tap::add("mutex_deadlock", t_mutex_deadlock);       // H6 self + cycle refusal
    tap::add("mutex_close_owned", t_mutex_close_owned); // R2 close-of-owned refused
    tap::add("mutex_multi_held", t_mutex_multi_held);   // H3 recompute vs restore-to-base
    tap::add("mutex_unlock_errors", t_mutex_unlock_errors); // non-owner / unlocked -> -1
    tap::add("mutex_owner_died_nowaiter", t_mutex_owner_died_nowaiter); // R3 no-waiter branch
    tap::add("mutex_deleg_refcount", t_mutex_deleg_refcount); // child close, parent still locks
    // Endpoint IPC (M3 #4 stage i): production syscalls, so runs on every board.
    tap::add("endpoint_rendezvous", t_endpoint_rendezvous); // both orderings + zero-len + truncation
    tap::add("endpoint_reject", t_endpoint_reject);         // F4 oversize + bad cap
    tap::add("endpoint_rights", t_endpoint_rights);         // send needs SIGNAL, recv needs WAIT
    tap::add("endpoint_epipe", t_endpoint_epipe);           // parked sender woken -1 on last WAIT close
    tap::add("endpoint_dead", t_endpoint_dead);             // F1 dead endpoint: send -> -1, no park
    tap::add("endpoint_crossdomain", t_endpoint_crossdomain); // F5 cross-domain copy + delegation
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    tap::add("endpoint_bound", t_endpoint_bound); // bound-check: bad recv/send buffer -> -1
#endif
    // Console handover mechanism (M3 #4 stage ii-a): production syscalls, every board.
    tap::add("cap_index0", t_cap_index0);              // B3 index-0 reservation (own create != slot 0)
    tap::add("console_publish_priv", t_console_publish); // D3 privileged-only + bad-cap reject
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
