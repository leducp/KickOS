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
        g_irqdrv_done = kos_sem_create(0);
        g_irqdrv_ready = kos_sem_create(0);
        g_mmio = kos_ram_alloc(4096);
        TAP_CHECK(g_mmio != nullptr);
        *static_cast<volatile int*>(g_mmio) = 0;
        int drv = kos::thread::spawn(irq_driver, nullptr, "irqdrv", 15, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false, g_mmio, 4096);
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
    tap::add("irq_ownership", t_irq_ownership);
    tap::add("irq_spurious", t_irq_spurious);
#if KICKOS_HAVE_MPU
    tap::add("mpu_privileged_guard", t_mpu_guard); // needs enforced protection
#endif
#endif

    // Every test joins its workers, so main returns as the last live thread:
    // the failure count becomes the process exit status (0 == all passed).
    return tap::run_all();
}
