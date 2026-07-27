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
#include <kickos/sys/bus.h> // M4.4 wire ABI: compile-checks the struct-size static_asserts here
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
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

    // Tests whose PREMISE is a privileged root, stated once here instead of restated at
    // each site. On a KICKOS_ROOT_PRIVILEGED=0 board this suite's orchestrator (main ==
    // root) is an ordinary unprivileged thread whose region set is [app code RX, app
    // static data RW, its own stack]. Two consequences reach the tests below:
    //
    //   1. It cannot spawn a privileged child. thread_spawn refuses that from an
    //      unprivileged caller, and that check is deliberately NOT a capability, because
    //      holding it would be equivalent to holding every authority forever.
    //   2. kos_ram_alloc reserves arena memory but GRANTS THE CALLER NOTHING. Allocation
    //      and grant are separate acts here: a region becomes reachable by being handed
    //      to a spawn, so root can allocate a page and then not touch it. A privileged
    //      root never noticed, holding the whole arena. This is a gap in the capability
    //      story rather than a defect in these tests -- an AUTH_MEMORY holder can
    //      allocate memory it cannot use -- and it is filed in TODO.md.
    //
    // Zero bytes in the default posture, deliberately: a runtime `if` would keep the skip
    // strings in .rodata on every board, and the fleet Debug default is `-g` with no -O
    // at all, so nothing would fold them away -- f302nucleo-st links with 96 bytes free.
    // NOTE the macro RETURNS from the enclosing test; that is what makes it a one-line
    // guard at the top of a test body, and why it is spelled in capitals.
#if KICKOS_ROOT_PRIVILEGED
#define SKIP_TEST_IF_ROOT_UNPRIVILEGED(why) do { } while (0)
#else
#define SKIP_TEST_IF_ROOT_UNPRIVILEGED(why)                                              \
    do                                                                                   \
    {                                                                                    \
        tap::skip("root unprivileged: " why);                                            \
        return;                                                                          \
    } while (0)
#endif

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

    // Self-contained probe worker: takes a slot and a stack, posts, exits. Nothing
    // waits on it, so any subset of a probe batch always drains.
    void pool_probe_worker(void*)
    {
        kos_sem_post(CH_DONE);
    }

    // Can this board host `n` workers CONCURRENTLY, right now?
    //
    // Most tests can spawn first and ask afterwards, because their workers each
    // self-complete: whoever got a slot posts g_done and a partial batch drains. A
    // choreography whose workers wait on EACH OTHER cannot -- the ones that did start
    // block forever on a rendezvous the missing ones would have completed, and the
    // drain in the "pool too small" arm never returns. That is not hypothetical: it is
    // how call_infoless_revert hung the armv6m gate from M4.4 to M4.5.1. Such a test
    // must ask BEFORE it spawns anything, and this is how.
    //
    // Probing beats reading a knob. The answer has to fold in the slots a board's
    // service-list drivers are already holding and whether the arena can still give
    // each worker a stack -- neither of which KICKOS_MAX_THREADS describes, and which
    // the app cannot see anyway (it is kernel-side config).
    //
    // Safe immediately before the real spawns: spawning does not itself reschedule, so
    // all `n` probes are resident at once (that is what makes this a CONCURRENCY test
    // rather than a repeated one-slot check), and root is the lowest-priority thread in
    // the system (kmain), so each probe runs all the way through exit before root is
    // scheduled again. By the time wait_n returns, every probe slot is EXITED
    // (reclaimable) and every probe stack is back on the pool's free list.
    bool pool_can_host(int n)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int got = 0;
        for (int i = 0; i < n; i++)
        {
            if (kos::thread::spawn_caps(pool_probe_worker, nullptr, "probe", 10, caps, 1) < 0)
            {
                break;
            }
            got++;
        }
        wait_n(got);
        return got == n;
    }

    // --- SVC argument/return roundtrip -----------------------------------------
    // PROVES: the kconsole_write SVC marshals a (buf, len) pair into the kernel and
    // brings the resulting byte count back out, on whatever trap mechanism the arch
    // uses (sim trampoline, ARM SVC, RISC-V ecall) -- and that the count comes from
    // the len WE passed, not from a kernel-side walk of the buffer.
    //
    // DOES NOT PROVE DELIVERY, and no longer claims to. kos_kconsole_write returns
    // `len` for any readable buffer whether or not console_emit then discarded every
    // byte -- which is exactly what it does once a board's service list hands the UART
    // to a userspace driver (kernel/init/console.cc, USER_OWNED) -- and userspace has
    // no readback. So this test was called "console_write roundtrip" while being
    // structurally incapable of noticing a dark console, and passed vacuously right
    // through the M4.5 silencing. Re-scoped rather than strengthened: delivery is only
    // observable where the transport ACKNOWLEDGES, i.e. the published stdout endpoint,
    // and that IS asserted -- by cap_index0's post-publish arm and by the harness's own
    // route probe, both of which key off a real rendezvous return. Concretely: the two
    // `# [svc] ...` lines below are ABSENT from a published board's log while this test
    // still reports `ok`. That absence IS the drop, and it is precisely why asserting on
    // the returned count alone can never be more than an ABI check.
    void t_svc()
    {
        char const* s = "# [svc] kconsole_write arg/return roundtrip (not a delivery check)\n";
        size_t const n = strlen(s);
        TAP_CHECK(kos_kconsole_write(s, n) == static_cast<long>(n));
        TAP_CHECK(kos_kconsole_write(s, 0) == 0); // a len-0 write is a legitimate 0 (sys.h)
        // len is honoured, not second-guessed: pass a PREFIX (itself a whole line, so the
        // TAP stream stays well formed) and require the short count back. A kernel that
        // strlen'd the buffer instead would return more -- and spill the tail marker.
        char const* pfx = "# [svc] len-honoured prefix\nTRAILING-MUST-NOT-APPEAR";
        long const cut = static_cast<long>(strlen("# [svc] len-honoured prefix\n"));
        TAP_CHECK(kos_kconsole_write(pfx, static_cast<size_t>(cut)) == cut);
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

    // Branch-clock oracle (M4.3): kos_periph_clock_hz.
    void t_periph_clock_hz()
    {
        // A base no backend models returns 0 on EVERY target, proving the dispatch
        // path + the weak/strong-override plumbing reach the arch seam. On the host
        // sim any base returns 0 (no silicon clock), mirroring cpu_clock_hz's sim-0.
        uint32_t const bogus = kos_periph_clock_hz(0xDEAD0000u);
        TAP_CHECK(bogus == 0u);
        TAP_CHECK(bogus == kos_periph_clock_hz(0xDEAD0000u)); // read-only + stable
    }

    // Pin-mux syscall (M4.3): kos_pinmux_set. An out-of-range port/pin is REJECTED
    // (rc < 0) on every target -- -KOS_EINVAL where a chip owns its PORT/IOCR block,
    // -KOS_ENOSYS on the weak-seam targets (host sim) -- so garbage is never silently
    // accepted and the dispatch -> arch-seam plumbing is proven. This runs from the
    // privileged root, but touches no hardware: both rejects return BEFORE any write.
    void t_pinmux_set()
    {
        TAP_CHECK(kos_pinmux_set(99u, 0u, 0x10u) < 0);  // port out of range
        TAP_CHECK(kos_pinmux_set(0u, 99u, 0x10u) < 0);  // pin out of range
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
            tap::skip("thread pool too small");
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
        // UNPRIVILEGED, like every other worker in this suite. These two were the only
        // privileged spawns left, and the privilege was never load-bearing: rr_worker
        // touches its delegated caps, the event log and g_rr_burn_ns (all app static data,
        // granted RW to any unprivileged thread) plus kos_clock_now, and not one of the
        // four assertions below is about privilege. The flag dates to the original TAP
        // harness, before a thread's region set was composed from its privilege at all,
        // and carried no rationale. Dropping it is what lets the WHOLE suite run under an
        // unprivileged root (thread_spawn refuses a privileged child from an unprivileged
        // caller, by design and not as a capability) without shedding a single check --
        // and RR over unprivileged threads is the shipping posture anyway, so this
        // exercises the region reload per slice that the privileged pair skipped.
        int a = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                                        caps, 2, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                        /*privileged=*/false);
        int b = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                                        caps, 2, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                        /*privileged=*/false);
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
    constexpr int IRQ_LINE = 7;

    void irq_driver(void*)
    {
        auto irq = kos::Irq::request(IRQ_LINE);
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
        // Root PLAYS THE DEVICE here -- it writes 0x101/0x102/0x103 into the driver's
        // granted page and injects the line -- so it needs write access to a page it
        // allocated but was never granted. That is consequence 2 above, and it is not
        // fixable by relabelling a flag: the mock register has to live in memory both
        // root and the driver can reach, and under the flip no such arena region exists
        // (app static data would reach both but is outside the arena, so it cannot be
        // granted, which is the very thing this test covers).
        SKIP_TEST_IF_ROOT_UNPRIVILEGED("root cannot write the driver's granted MMIO page "
                                       "(kos_ram_alloc grants its caller nothing)");
        // Alloc before the sems: an alloc-fail early return must not leak them (pool-honest suite).
        g_mmio = kos_ram_alloc(4096);
        if (g_mmio == nullptr)
        {
            // A tiny RAM arena (microbit: 16 KiB SRAM) cannot spare a 4 KiB page for the
            // mock MMIO region: a real TAP SKIP (counted), like the pool-too-small skips below.
            tap::skip("4 KiB MMIO-page alloc failed -- board too small");
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
            kos_irq_inject(IRQ_LINE);
            kos_sem_wait(g_irqdrv_done); // serviced + acked
        }
        kos_sem_destroy(g_irqdrv_done); // reclaim (driver has exited: higher prio ran to completion)
        kos_sem_destroy(g_irqdrv_ready);
        TAP_CHECK(g_seen[0] == 0x101 and g_seen[1] == 0x102 and g_seen[2] == 0x103);
    }

    // --- IRQ mask latches-and-coalesces a masked raise -------------------------
    // The driver runs BELOW root's priority, so posting its notification does not
    // preempt root: root can fire three back-to-back. The first fire delivers and
    // masks the line; the second and third land on the masked line and COALESCE
    // one-deep -- so the driver services EXACTLY twice (never a phantom third), the
    // single latch redelivered when ack unmasks the line.
    int g_mask_ready = -1;
    int g_mask_serviced = 0;
    constexpr int MASK_LINE = 6;

    void mask_driver(void*)
    {
        auto irq = kos::Irq::request(MASK_LINE);
        kos_sem_post(CH_READY); // g_mask_ready
        for (int i = 0; i < 3; i++)
        {
            irq.wait();
            g_mask_serviced++;
            irq.ack(); // unmask redelivers the one coalesced latch (2nd iteration)
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
        kos_sem_wait(g_mask_ready); // line registered + armed, driver parked in wait
        kos_sem_destroy(g_mask_ready);
        // Three back-to-back onto the parked (lower-prio) driver's line: #1 delivers
        // + masks; #2 latches on the masked line; #3 coalesces into that one latch.
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        // Deterministic: block until the two services (the delivery + the single
        // coalesced redelivery) have both landed -- no sleep-based ordering here.
        wait_n(2);
        // Bounded settle: a spurious third wake (a dropped-then-phantom regression)
        // would bump serviced past 2 while the driver is parked in its third wait.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_mask_serviced == 2); // exactly two; the third raise coalesced
        // Release the parked third wait with a fresh event so the driver exits + joins.
        kos_irq_inject(MASK_LINE);
        wait_n(1);
        TAP_CHECK(g_mask_serviced == 3);
    }

    // --- Auto-rearm: wait; service with NO explicit ack ------------------------
    // irq_wait re-arms the previously-consumed line itself, so a driver that never
    // acks still receives every subsequent IRQ. Driver runs ABOVE root (like
    // t_irqdrv) so it re-arms (reaches its next wait) before root injects again --
    // a raise onto a still-masked line would latch-and-coalesce, not be lost.
    int g_autorearm_ready = -1;
    int g_autorearm_seen = 0;
    constexpr int AUTO_REARM_LINE = 8;

    void autorearm_driver(void*)
    {
        auto irq = kos::Irq::request(AUTO_REARM_LINE);
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
            kos_irq_inject(AUTO_REARM_LINE);
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
    constexpr int PHANTOM_LINE = 10;

    void phantom_driver(void*)
    {
        auto irq = kos::Irq::request(PHANTOM_LINE);
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

        kos_irq_inject(PHANTOM_LINE); // fire #1
        wait_n(1);                    // driver consumed #1 and acked (line armed)

        kos_irq_inject(PHANTOM_LINE); // the one mid-compute event, on the armed line
        wait_n(1);                    // serviced exactly once
        TAP_CHECK(g_phantom_seen == 1);

        // The driver is now parked in its third wait. It is lower priority, so
        // sleeping yields the CPU to it: a phantom wake would bump seen here.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_phantom_seen == 1); // second wait genuinely blocked -> no phantom

        // Prove that wait is live (blocked, not lost) and the line re-armed itself:
        // a fresh inject delivers.
        kos_irq_inject(PHANTOM_LINE);
        wait_n(1);
        TAP_CHECK(g_phantom_seen == 2);
    }

#endif // KICKOS_ENABLE_SELFTEST (tier-1 IRQ + mask)

    // --- Semaphore destroy: freelist reuse + generation-tagged handles ---------
    void t_sem_destroy()
    {
        int h = kos_sem_create(0);
        TAP_CHECK(h >= 0);
        TAP_CHECK(kos_sem_destroy(h) == 0);          // live handle destroys
        TAP_CHECK(kos_sem_destroy(h) == -KOS_EBADF); // stale handle rejected (gen bumped)
        int h2 = kos_sem_create(0);
        TAP_CHECK(h2 >= 0 and h2 != h); // reused slot carries a fresh generation
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        // Malformed / out-of-range caps at the resolve boundary must fail-safe with the
        // SPECIFIC code -KOS_EBADF (bad index / empty / stale gen): negative, garbage-huge,
        // and an out-of-range index all reject. Via handle_close (the one cap syscall that
        // returns a value; wait/post share the same cap_resolve chokepoint). Pool-neutral.
        TAP_CHECK(kos_handle_close(-1) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -KOS_EBADF);
        // The count is bounded at BOTH ends. A sem may not be born outside
        // [0, KOS_SEM_COUNT_MAX], and a post with no waiter and the count already at the
        // ceiling is refused instead of incrementing an int past its range -- which is UB,
        // and post is reachable from unprivileged code. Creating one AT the ceiling is what
        // makes the refusal reachable in a test at all: the alternative is 2^31 posts.
        TAP_CHECK(kos_sem_create(-1) == -KOS_EINVAL);
        int const hmax = kos_sem_create(KOS_SEM_COUNT_MAX);
        TAP_CHECK(hmax >= 0 and kos_sem_post(hmax) == -KOS_EOVERFLOW
                  and kos_handle_close(hmax) == 0);
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
            tap::skip("pool too small");
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
            tap::skip("pool too small");
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
            tap::skip("pool too small");
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
        g_od_result = kos_mutex_lock(2); // block; woken by the dying owner with -KOS_EOWNERDEAD
        // -KOS_EOWNERDEAD is a HELD acquire (owner died): unlock it too, or the robust
        // mutex would be stranded. A plain `>= 0` test would wrongly skip this now that
        // owner-died is a NEGATIVE code -- special-case it as held.
        if (g_od_result == 0 or g_od_result == -KOS_EOWNERDEAD)
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
        TAP_CHECK(g_od_result == -KOS_EOWNERDEAD); // acquired-but-owner-died (held, negative code)
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
        // Self-deadlock: a recursive lock is refused (-KOS_EDEADLK), not parked, and leaves
        // the mutex holdable/releasable normally.
        int self = kos_mutex_create();
        TAP_CHECK(self >= 0);
        TAP_CHECK(kos_mutex_lock(self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == -KOS_EDEADLK); // recursive -> refused
        TAP_CHECK(kos_mutex_unlock(self) == 0);
        TAP_CHECK(kos_handle_close(self) == 0);

        // Cross-thread cycle: A owns M1 + waits M2; B owns M2 + tries M1 -> -KOS_EDEADLK.
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
            // cap table (MAX_HANDLES=9, 3 free) / sem pool (MAX_SEMAPHORES=4, 2 free) can't
            // hold them. No worker has spawned yet, so just reclaim what was created (in
            // any order -- close/destroy ignores a <0 handle) and skip.
            if (m1 >= 0) { kos_handle_close(m1); }
            if (m2 >= 0) { kos_handle_close(m2); }
            if (have1 >= 0) { kos_sem_destroy(have1); }
            if (have2 >= 0) { kos_sem_destroy(have2); }
            if (goA >= 0) { kos_sem_destroy(goA); }
            if (goB >= 0) { kos_sem_destroy(goB); }
            tap::skip("pool too small");
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
        TAP_CHECK(g_cyc_rb == -KOS_EDEADLK); // B closing the cycle -> refused
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
        TAP_CHECK(kos_handle_close(m) == -KOS_EBUSY); // refused: you cannot close a mutex you hold
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
            tap::skip("pool too small");
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
        g_nonowner_rc = kos_mutex_unlock(2); // caller is not the owner -> -KOS_EPERM
        kos_sem_post(CH_DONE);
    }
    void t_mutex_unlock_errors()
    {
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0);
        TAP_CHECK(kos_mutex_unlock(m) == -KOS_EPERM); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2
        int w = kos::thread::spawn_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_nonowner_rc == -KOS_EPERM); // non-owner unlock refused, no panic
        TAP_CHECK(kos_mutex_unlock(m) == 0);    // the real owner still unlocks
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
        // The one test whose subject IS the privileged posture: the guarded page is
        // granted to no domain, so only a whole-arena thread reaches it. Under the flip no
        // privileged thread can come into existence after boot at all, so the property
        // does not fail here -- it does not exist. Its complement, that root can NOT reach
        // memory outside its regions, is apps/rootfault, which must be a separate binary
        // because proving it ends the process.
        SKIP_TEST_IF_ROOT_UNPRIVILEGED("no privileged caller exists; the inverse claim is "
                                       "apps/rootfault");
        volatile int* g = static_cast<volatile int*>(kos_guard_addr());
        *g = 0x1234; // privileged (root): guard is RW, must not fault
        kos_yield(); // a syscall must restore the caller's MPU posture, not PROT_NONE
        TAP_CHECK(*g == 0x1234);
    }
#endif

    // --- One driver per line: a second claim on a bound line is refused --------
    void t_irq_ownership()
    {
        constexpr int LINE = 11; // unused by the other IRQ tests
        int sem = kos_sem_create(0);
        TAP_CHECK(kos_irq_attach(LINE, sem) == 0);          // first claim wins
        TAP_CHECK(kos_irq_attach(LINE, sem) == -KOS_EBUSY); // second is refused (no steal)
        TAP_CHECK(kos_irq_register(LINE) == -KOS_EBUSY);    // tier-1 cannot steal it either
        kos_sem_destroy(sem); // reclaim (line 11 stays bound to a now-stale handle -> fails safe)
    }

    // --- Spurious IRQ: an unbound line is masked + counted, never dropped -------
    void t_irq_spurious()
    {
        constexpr int FREE_LINE = 9; // no driver bound to this line
        // Enable the line so the injected raise reaches the default handler on
        // masked-by-default controllers (ARM NVIC, RX); sim/riscv are unmasked by
        // default, so this is a no-op there.
        kos_irq_unmask(FREE_LINE);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(FREE_LINE);   // default handler runs: mask + bump counter
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        // The default handler masked the line, so a second raise LATCHES on the
        // masked line (coalesced, not delivered): the handler does not re-run, so
        // the counter must NOT advance until the line is unmasked again.
        kos_irq_inject(FREE_LINE);
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
    }

    // --- First-arm discards pre-registration garbage ---------------------------
    // A raise that lands before a driver owns the line is latched (the coalesce
    // contract). irq_register must DISCARD that stale latch at first-arm (arch_irq_
    // clear_pending) -- else the very first irq.wait() would phantom-wake on garbage
    // the driver never asked for. Root leaves a latch on an unbound line, then a
    // lower-prio driver registers + waits: that first wait MUST block.
    int g_stale_ready = -1;
    int g_stale_seen = 0;
    constexpr int STALE_LINE = 12;

    void stale_driver(void*)
    {
        auto irq = kos::Irq::request(STALE_LINE); // first-arm clears the stale latch
        kos_sem_post(CH_READY);                   // g_stale_ready
        irq.wait();                               // MUST block: the garbage was discarded
        g_stale_seen++;                           // only after root injects a REAL event
        kos_sem_post(CH_DONE);
    }
    void t_irq_stale_register()
    {
        g_stale_ready = kos_sem_create(0);
        g_stale_seen = 0;
        // Pre-registration garbage on the unbound line: unmask so the default handler
        // runs (mask + count) on the first raise, then a second raise latches on the
        // now-masked line -- the stale pending that first-arm must discard.
        kos_irq_unmask(STALE_LINE);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(STALE_LINE); // default handler: mask + count
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        kos_irq_inject(STALE_LINE); // masked now -> latches garbage (pre-registration)
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_stale_ready, CH_FULL}}; // done@1, ready@2
        int drv = kos::thread::spawn_caps(stale_driver, nullptr, "staleirq", 1, caps, 2); // below root
        TAP_CHECK(drv >= 0);         // spawn failure would hang the ready handshake below
        kos_sem_wait(g_stale_ready); // driver registered (latch discarded) + parked in wait
        kos_sem_destroy(g_stale_ready);
        // No phantom: the driver's first wait blocks despite the pre-registration latch.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_stale_seen == 0);
        // Liveness: a real event delivers on the freshly-armed line, driver exits + joins.
        kos_irq_inject(STALE_LINE);
        wait_n(1);
        TAP_CHECK(g_stale_seen == 1);
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
    // 2048, generously above the per-arch KICKOS_MIN_STACK_SIZE floor: the worker runs the
    // whole deepest kernel dispatch (syscall trap frame + thread-exit teardown) on THIS
    // stack. Matches the dynamic caller stack (STK) below, which runs the same worker + exit
    // path. (The floor is now sized to that deepest dispatch per arch; 2048 clears it easily.)
    KOS_STACK_DEFINE(g_cstk_static, 2048);
#endif
    void t_caller_stack()
    {
        // Reject a non-null, tiny + misaligned caller stack: -KOS_EINVAL, not run or corrupt.
        TAP_CHECK(kos::thread::spawn(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8)
                  == -KOS_EINVAL);
        // Accept a properly-sized, aligned caller-owned stack -> the thread runs on it.
        // Drop this half when the arena can't spare one (tiny-RAM parts, like test 11's
        // alloc): the API is arch-uniform; this only needs the memory to demonstrate it.
        // The reject case above already ran, so the test stays `ok` and says which half
        // it dropped -- it used to return here in total silence.
        constexpr uint32_t STK = 2048;
        void* raw = kos_ram_alloc(STK + 16);
        if (raw == nullptr)
        {
            tap::diag("caller_stack: PARTIAL -- accept half not run (arena cannot spare a stack)");
            return;
        }
        void* stk = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
        // Reject a PROPERLY-ALIGNED but sub-floor caller stack -- the exact bug the per-arch
        // floor fixes: an aligned 512 B stack once passed the check, then overflowed the
        // RISC-V exit dispatch (~624 B). One alignment unit below the floor, aligned base:
        // the size check must reject it BEFORE any slot / region work. (16 = KICKOS_STACK_ALIGN.)
        TAP_CHECK(kos::thread::spawn(caller_stack_worker, nullptr, "undf", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, stk, KICKOS_MIN_STACK_SIZE - 16u,
                                     nullptr, 0, nullptr, 0)
                  == -KOS_EINVAL);
        g_cstk_sem = kos_sem_create(0);
        kos_cap_grant caps[] = {{g_cstk_sem, CH_FULL}}; // -> g_cstk_sem @1 (CH_DONE)
        int const t = kos::thread::spawn(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, stk, STK, nullptr, 0, caps, 1);
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
    constexpr int DOM_SENTINEL = 0x5A5A;
    void dom_writer(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        *g_dshared = DOM_SENTINEL; // write the shared domain region (granted)
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
        // Nothing to assert on a part whose arena cannot spare the region, so this is a
        // real SKIP, not a pass (it used to return here silently and count as `ok`).
        // Alloc before the sems so an early return leaks nothing.
        g_dshared = static_cast<volatile int*>(kos_ram_alloc(256));
        if (g_dshared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // No pre-zero of the region here. It asserted nothing -- the only check is
        // g_dreadback == DOM_SENTINEL, g_dreadback is written only by the reader, and the
        // reader runs only after the writer's post -- so a stale word could never fake the
        // sentinel. Dropping it is also what keeps this test running in BOTH root
        // postures: an unprivileged root cannot write a region it allocated and has not
        // been granted, and the region is the workers' to initialise, not the
        // orchestrator's.
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
            // a real TAP SKIP. sim + qemu (larger pools) exercise the shared domain.
            // Any worker that did spawn self-completes (writes, posts, returns->exits).
            tap::skip("thread pool too small for 2 concurrent");
            kos_sem_destroy(g_dwrote);
            kos_sem_destroy(g_dread);
            return;
        }
        kos_sem_wait(g_dread); // the reader saw the writer's store via the shared region
        kos_sem_destroy(g_dwrote);
        kos_sem_destroy(g_dread);
        TAP_CHECK(g_dreadback == DOM_SENTINEL);
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
        // Privileged caller, non-encodable window (size 1, unaligned base): rejected with
        // -KOS_EINVAL, not rounded.
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmiobad", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x1001u), 1) == -KOS_EINVAL);
        // A non-null base with size 0 is rejected at the boundary (before domain_for).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmio0", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x2000u), 0) == -KOS_EINVAL);
        // A window whose base+size wraps the address space is rejected -KOS_EINVAL (32-bit
        // MCU; on the 64-bit sim the fail-closed encoder rejects it first -- either way EINVAL).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmioW2", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0xFFFFFFF0u), 0x20) == -KOS_EINVAL);
        g_mmio_done = kos_sem_create(0);
        g_mmio_unpriv_rc = -2;
        kos_cap_grant caps[] = {{g_mmio_done, CH_FULL}}; // g_mmio_done@1 (CH_DONE)
        int w = kos::thread::spawn_caps(mmio_unpriv_worker, nullptr, "mmioW", 10, caps, 1);
        if (w < 0)
        {
            // Tiny thread pool (e.g. microbit MAX_THREADS=2). The three privileged
            // encodability cases above already ran, so this stays `ok` and names the half
            // it dropped -- a whole-test SKIP would understate what was proven.
            tap::diag("mmio_grant: PARTIAL -- unprivileged half not run (thread pool too small)");
            kos_sem_destroy(g_mmio_done);
            return;
        }
        kos_sem_wait(g_mmio_done);
        kos_sem_destroy(g_mmio_done);
        TAP_CHECK(g_mmio_unpriv_rc == -KOS_EPERM); // unprivileged MMIO self-grant refused
    }

    // --- stack_base arena containment (unprivileged self-grant) -----------------
    // The stack_base grant is the ONE unprivileged path that reaches an MPU region:
    // thread.cc commits a caller-owned stack as one R|W region. Without an arena bound
    // an unprivileged thread spawns a child with stack_base in peripheral space or
    // kernel SRAM -- an R|W window the MMIO gate would refuse. Enforcing backends only
    // (the check is MPU-gated; with no region descriptor there is no escalation).
#if KICKOS_HAVE_MPU
    int g_stkarena_rc = -2;
    int g_stkarena_done = -1;
    void stkarena_noop(void*) {}
    void stkarena_unpriv_worker(void*)
    {
        // Unprivileged caller; stack_base far above any SRAM arena and naturally aligned
        // (clears the size/align/natural checks) so ONLY the arena bound can reject it.
        g_stkarena_rc = kos::thread::spawn(stkarena_noop, nullptr, "stkbad", 10,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                           reinterpret_cast<void*>(0xE0000000u), 2048);
        kos_sem_post(CH_DONE); // g_stkarena_done
    }
    void t_stackbase_arena()
    {
        g_stkarena_done = kos_sem_create(0);
        g_stkarena_rc = -2;
        kos_cap_grant caps[] = {{g_stkarena_done, CH_FULL}}; // g_stkarena_done@1 (CH_DONE)
        int w = kos::thread::spawn_caps(stkarena_unpriv_worker, nullptr, "stkW", 10, caps, 1);
        if (w < 0)
        {
            // The unprivileged child IS this test -- there is no privileged half to fall
            // back on -- so a tiny thread pool (microbit MAX_THREADS=2) leaves nothing to
            // assert: a whole-test SKIP, not a partial.
            tap::skip("thread pool too small");
            kos_sem_destroy(g_stkarena_done);
            return;
        }
        kos_sem_wait(g_stkarena_done);
        kos_sem_destroy(g_stkarena_done);
        TAP_CHECK(g_stkarena_rc == -KOS_EPERM); // out-of-arena unprivileged stack_base refused
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Rule 7 grant predicates: the overlap matrix + RAM/DEV admission ---------
    // Exercises grant_hits_reserved / grant_region_admissible directly (kos_grant_probe,
    // test-only). The RAM-path cases run wherever the arena exists (sim). The reserved-
    // OVERLAP matrix needs a board that actually declares reserved blocks; the runnable
    // MPU board (sim) reserves nothing, so that half reports PARTIAL there (the test
    // still asserts the RAM/DEV admission cases, so it is NOT a whole-test SKIP) and runs
    // for real on an enforcing MCU. (The bit-band alias-hit case needs a bit-band M4 and
    // is HW-only.)
    void grant_noop(void*) {}
    void t_grant_reserved()
    {
        // --- RAM-path admission (arena-relative; runs on sim). ---
        // kos_ram_alloc hands back a block naturally aligned to its rounded region
        // size, so it is admissible R|W for EVERY caller posture (10C, no waiver).
        void* raw = kos_ram_alloc(2048);
        if (raw != nullptr)
        {
            uintptr_t const a = reinterpret_cast<uintptr_t>(raw);
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a, 2048) == 1);       // in-arena, aligned, privileged
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_UNPRIVILEGED, a, 2048) == 1);     // in-arena, aligned, unprivileged
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a + 16, 2048) == 0);  // R1: base not aligned to the region size
        }
        else
        {
            tap::diag("grant_reserved: PARTIAL -- arena-relative RAM cases not run (2 KiB alloc failed)");
        }
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x1000u, 0x1000u) == 0);      // out-of-arena RAM refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0xFFFFFFF0u, 0x20u) == 0);    // wrap (32-bit) / out-of-arena (64-bit) refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x20000000u, 0u) == 0);       // size 0 refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_UNPRIVILEGED, 0x40000000u, 0x1000u) == 0);  // DEV grant, unprivileged caller: refused

        // --- End-to-end errno coherence (MAJOR 2): an unprivileged child whose mem_base
        // lies OUTSIDE the arena is refused with -KOS_EPERM (policy refusal), NOT
        // -KOS_ENOMEM (pool exhaustion) -- coherent with the stack_base path
        // (t_stackbase_arena). domain_for is the authoritative chokepoint and now reports
        // WHICH refusal it made, so this code comes from there rather than from a
        // duplicate pre-check at the spawn boundary. Caller is privileged (main); the CHILD is
        // unprivileged, so domain_for evaluates the grant (0xE0000000 is 2048-aligned, so
        // ONLY arena containment can reject it). Fails before any slot is claimed.
        int const mrc = kos::thread::spawn(grant_noop, nullptr, "membad", 10, KOS_POLICY_FIFO,
                                           0, /*privileged=*/false,
                                           reinterpret_cast<void*>(0xE0000000u), 2048);
        TAP_CHECK(mrc == -KOS_EPERM); // out-of-arena data grant: policy refusal, never ENOMEM

        // --- Reserved-overlap matrix. ---
        // The OVERLAP cases anchor on block[0] and are layout-independent (a window that
        // overlaps block[0] hits regardless of neighbours). The NON-overlap cases must
        // land in a GAP, so they anchor on scanned edges: the lowest reserved base has
        // nothing flush below it, and the highest reserved end has nothing at-or-above
        // it -- guaranteed by min/max, even when blocks are flush (rp2040 TIMER abuts
        // WATCHDOG). Testing block[0]-relative "above" would false-hit on such a board.
        uintptr_t const n = kos_grant_probe(KOS_GRANT_OP_RESERVED_COUNT, 0, 0);
        if (n == 0)
        {
            tap::diag("grant_reserved: PARTIAL -- reserved-overlap matrix not run (board reserves nothing)");
            return;
        }
        uintptr_t const rb = kos_grant_probe(KOS_GRANT_OP_RESERVED_BASE, 0, 0);       // block[0].base
        uintptr_t const rs = kos_grant_probe(KOS_GRANT_OP_RESERVED_SIZE, 0, 0);       // block[0].size
        uintptr_t const rlast = rb + rs - 1u;
        // Scan for the lowest base and highest end across the whole set.
        uintptr_t lo_base = rb;
        uintptr_t hi_end = rb + rs; // one-past-last
        for (uintptr_t i = 0; i < n; i++)
        {
            uintptr_t const b = kos_grant_probe(KOS_GRANT_OP_RESERVED_BASE, i, 0);
            uintptr_t const s = kos_grant_probe(KOS_GRANT_OP_RESERVED_SIZE, i, 0);
            if (b < lo_base)
            {
                lo_base = b;
            }
            if (b + s > hi_end)
            {
                hi_end = b + s;
            }
        }
        // Refuse (overlap): equal, contained, partial straddle, both one-byte edges --
        // all overlap block[0], so hit regardless of layout.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb, rs) == 1);            // equal
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb + 4u, 8u) == 1);       // contained
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb - 4u, 8u) == 1);       // partial straddle (low edge)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb - 1u, 2u) == 1);       // one-byte edge low (last == rb)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rlast, 2u) == 1);         // one-byte edge high (base == rlast)
        // Permit (no overlap), anchored on proven gap edges:
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, lo_base - 0x10u, 0x10u) == 0); // adjacent below lowest (last == lo_base-1)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end, 0x10u) == 0);          // adjacent above highest (base == prev last+1)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end + 0x100000u, 0x10u) == 0); // disjoint, well clear above
        // The gap edges are genuinely adjacent: the reserved byte just inside each edge
        // still hits (proves the boundary is exact, not merely that the gap is empty).
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, lo_base, 1u) == 1);       // first byte of the lowest block
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end - 1u, 1u) == 1);   // last byte of the highest block
        // Rule 7 core: a reserved block is inadmissible as a DEV grant (privileged too)
        // and as RAM.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, rb, rs) == 0);            // DEV over reserved, privileged: refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, rb, rs) == 0);            // RAM over reserved, privileged: refused
        // End-to-end: a privileged spawn granting the reserved MMIO window is refused.
        int const rc = kos::thread::spawn(grant_noop, nullptr, "rsvd", 10, KOS_POLICY_FIFO,
                                          0, false, nullptr, 0, nullptr, 0,
                                          reinterpret_cast<void*>(rb),
                                          static_cast<uint32_t>(rs));
        TAP_CHECK(rc < 0); // reserved-block MMIO grant refused (domain_for, or non-encodable at the boundary)
    }
#endif // KICKOS_ENABLE_SELFTEST
#endif // KICKOS_HAVE_MPU

    // --- Confused-deputy readable-buffer floor ---------------------------------
    // syscall_dispatch runs privileged and bypasses the MPU, so a user pointer it
    // READS (the kconsole_write buffer, a thread name) must lie in memory the
    // UNPRIVILEGED caller could itself reach. A rodata string literal lives in the
    // app's code/rodata (a real MPU region on HW, the host image on the sim) and
    // MUST be accepted; a pointer into no granted region (the un-owned guard page)
    // MUST be rejected, never read. All checks run from an UNPRIVILEGED worker
    // (main is privileged and bypasses the floor).
    // WHAT THE POSITIVE HALF PROVES: that the floor ACCEPTED an unprivileged caller's
    // rodata pointer -- not that the bytes reached a console. kos_kconsole_write returns
    // `len` for every accepted buffer even when console_emit then drops all of it (any
    // published board), so a delivery claim here would be vacuous, and the old literal
    // ("...reaches the console") made exactly that claim. Its real force comes from
    // being PAIRED with the guard-page negative below: same syscall, same unprivileged
    // caller, -KOS_EFAULT -- together they show the floor discriminates reachable from
    // unreachable. Where no guard page exists (no enforcement) the positive stands alone
    // and is correspondingly weaker; that is inherent, not an oversight.
    char const CD_LIT[] = "# [confdep] unpriv rodata buffer accepted by the readable floor\n";
    long g_cd_lit_rc = -99;    // worker: kconsole_write(rodata literal) -> expect len (accepted, not delivered)
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
        g_cd_lit_rc = kos_kconsole_write(CD_LIT, strlen(CD_LIT)); // rodata: accepted

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
            tap::skip("thread pool too small");
            kos_sem_destroy(g_cd_done);
            return;
        }
        kos_sem_wait(g_cd_done);
        kos_sem_destroy(g_cd_done);
        // Positive (every backend): the floor accepted an unprivileged caller's rodata
        // pointer and read exactly len bytes from it. See CD_LIT on why this is an
        // acceptance check and deliberately not a delivery check.
        TAP_CHECK(g_cd_lit_rc == static_cast<long>(sizeof(CD_LIT) - 1));
        // Positive: a child named from .rodata spawned and ran (the name-copy path works).
        // The grandchild needs its own stack; on a tiny arena (microbit: 16 KiB SRAM, which
        // already cannot spare irq_as_event's 4 KiB page) that alloc can fail. Skip the
        // grandchild-name half there rather than fail -- the rodata-literal positive above
        // already exercised the confused-deputy read path; the name-copy path stays covered
        // on the roomier sim/qemu backends.
        if (g_cd_goodspawn < 0)
        {
            tap::diag("confused_deputy: PARTIAL -- grandchild-name half not run (arena too small for its stack)");
            return;
        }
        TAP_CHECK(g_cd_goodname_ran == 1);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        // Negative (enforcing backend): a bogus buffer/name is rejected, never read,
        // and never faults the kernel.
        if (g_cd_neg_ran)
        {
            TAP_CHECK(g_cd_bad_rc == -KOS_EFAULT); // bogus buffer rejected, never read (was 0)
            TAP_CHECK(g_cd_badname_spawn >= 0 and g_cd_badname_ran == 1);
        }
#endif
    }

    // --- Endpoint IPC: synchronous rendezvous send/recv (M3 #4 stage i) ----------
    // The endpoint cap is delegated to workers at child index 2 (done@1, E@2). Workers
    // are UNPRIVILEGED so the kernel's copy into/from a parked peer runs against real
    // enforcement (the cross-domain privileged write, design section 3.1).
    char const EP_MSG[] = "hello-endpoint"; // 14 bytes (strlen), no NUL sent
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
        // The recv buffer is a STACK local, and the result reaches main through a global
        // by a direct store rather than by the syscall. A global buffer would be accepted
        // now that user_writable_ok has a static-data fallback (see writable_global), but
        // keeping the buffer thread-private is what keeps this test about the rendezvous
        // instead of about the writable check.
        char buf[64];
        struct kos_recv_info info = {0xdeadu, 0x55};
        long n = kos_recv(2, buf, static_cast<size_t>(g_ep_rcap), &info);
        g_ep_rn = n;
        g_ep_rbadge = info.badge;
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
        g_ep_sn = kos_send(2, EP_MSG, strlen(EP_MSG));
        kos_sem_post(CH_DONE);
    }

    void t_endpoint_rendezvous()
    {
        size_t const mlen = strlen(EP_MSG);
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}}; // done@1, E@2

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rcap = 64;
        int w = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eprx", 12, caps, 2,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w >= 0);
        kos_sleep_ns(3000000ull); // let the worker park in recv
        long sc = kos_send(g_ep, EP_MSG, mlen);
        TAP_CHECK(sc == static_cast<long>(mlen)); // sender delivered n bytes
        wait_n(1);
        TAP_CHECK(g_ep_rn == static_cast<long>(mlen) and memcmp(g_ep_rbuf, EP_MSG, mlen) == 0);
        TAP_CHECK(g_ep_rbadge == 0); // badge always written on success (stage i: 0)

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        int w2 = kos::thread::spawn_caps(ep_send_worker, nullptr, "eptx", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w2 >= 0);
        kos_sleep_ns(3000000ull); // let the worker park in send
        char rbuf[64];
        struct kos_recv_info info = {0xdeadu, 0x55};
        long rc = kos_recv(g_ep, rbuf, sizeof(rbuf), &info);
        TAP_CHECK(rc == static_cast<long>(mlen) and memcmp(rbuf, EP_MSG, mlen) == 0);
        TAP_CHECK(info.badge == 0);
        wait_n(1);
        TAP_CHECK(g_ep_sn == static_cast<long>(mlen));

        // (C) zero-length is a valid signal (n == 0 on both sides, NOT -1).
        g_ep_rn = -99; g_ep_rcap = 64;
        int w3 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "epz", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w3 >= 0);
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, 0) == 0);
        wait_n(1);
        TAP_CHECK(g_ep_rn == 0);

        // (D) truncation: send mlen into a 4-byte capacity -> both return 4.
        g_ep_rn = -99; g_ep_rcap = 4;
        int w4 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eptr", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w4 >= 0);
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, mlen) == 4);
        wait_n(1);
        TAP_CHECK(g_ep_rn == 4 and memcmp(g_ep_rbuf, EP_MSG, 4) == 0);

        TAP_CHECK(kos_handle_close(g_ep) == 0); // last cap -> endpoint freed
    }

    // --- Oversize reject + bad cap (main only; no parking) -----------------------
    void t_endpoint_reject()
    {
        char big[KOS_EP_MSG_MAX + 8];
        memset(big, 'x', sizeof(big));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        // Oversize send is rejected up front (F4) with -KOS_EINVAL, WITHOUT parking (main is
        // the sole WAIT holder, so a park would hang the suite).
        TAP_CHECK(kos_send(g_ep, big, KOS_EP_MSG_MAX + 1) == -KOS_EINVAL);
        // Bad caps reject at the resolve boundary on both paths with -KOS_EBADF.
        char one[1] = {0};
        TAP_CHECK(kos_send(0x7fffffff, one, 1) == -KOS_EBADF);
        TAP_CHECK(kos_recv(0x7fffffff, g_ep_rbuf, 1, nullptr) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Rights denial: send needs SIGNAL, recv needs WAIT -----------------------
    volatile int g_ep_wait_send_rc = -99;   // WAIT-only cap send -> -1
    volatile int g_ep_signal_recv_rc = -99; // SIGNAL-only cap recv -> -1
    void ep_rights_worker(void*) // caps: done@1, E(WAIT)@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        g_ep_wait_send_rc = static_cast<int>(kos_send(2, b, 1));   // WAIT-only -> no SIGNAL -> -KOS_EPERM
        g_ep_signal_recv_rc = static_cast<int>(kos_recv(3, b, sizeof(b), nullptr)); // no WAIT -> -KOS_EPERM
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
        TAP_CHECK(g_ep_wait_send_rc == -KOS_EPERM);   // send refused without SIGNAL
        TAP_CHECK(g_ep_signal_recv_rc == -KOS_EPERM); // recv refused without WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- EPIPE: a parked sender is woken -1 when the last WAIT-cap holder drops it -
    // A SIGNAL-only delegation does NOT bump recv_holders, so main's cap is the sole
    // WAIT holder: closing it takes recv_holders 1->0 and EPIPEs the parked sender.
    volatile long g_ep_epipe_rc = -99;
    void ep_epipe_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_ep_epipe_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks; woken -KOS_EPIPE on EPIPE
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
        TAP_CHECK(g_ep_epipe_rc == -KOS_EPIPE); // sender woken with EPIPE, not a byte count
    }

    // --- Dead endpoint (unparked): send after the last WAIT cap is gone -> -1 -----
    // Distinct from the parked-then-EPIPE case: the sender never parks (F1 dead-check).
    volatile long g_ep_dead_rc = -99;
    int g_ep_go = -1;
    void ep_dead_worker(void*) // caps: done@1, E(SIGNAL)@2, go@3
    {
        kos_sem_wait(3);                                     // go: main has dropped its WAIT cap
        g_ep_dead_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // recv_holders == 0 -> -KOS_EPIPE now
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
        TAP_CHECK(g_ep_dead_rc == -KOS_EPIPE); // dead endpoint rejected immediately, never parked
        kos_sem_destroy(g_ep_go);
    }

    // --- Call/reply (M4.4): info-less receiver bounces a call, D2 boost is REVERTED ------
    // A high caller's slow-path kos_call boosts the low server it targets (D2). If the
    // server took an INFO-LESS recv it cannot host the call: recv rejects the caller
    // (-KOS_ENOSYS) and MUST revert that boost. Observed exactly as the PI donation test:
    // the boosted server holds the CPU past a medium spoiler's wake ('u' before 'm'), then
    // once reverted the spoiler runs before the server resumes ('m' before 'z'). Without
    // the revert the server stays pinned above the spoiler and 'z' precedes 'm'.
    uint64_t g_call_unit = 1000000ull;
    volatile long g_ci_rc = -99;
    void ci_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        kos_sleep_ns(g_call_unit * 1);         // let the two plain senders park first
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#1 (info-less): eats one plain sender; ep->server = us
        log_put('a');
        mtx_spin(g_call_unit * 4);             // hold the CPU: the caller wakes + D2-boosts us here
        log_put('u');
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#2: reject the parked call (deflate us), eat the 2nd sender
        log_put('z');                          // reached at base prio: spoiler ran first IFF we reverted
        kos_sem_post(CH_DONE);
    }
    void ci_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(g_call_unit * 2);         // wake mid-spin: slow-path call D2-boosts the server
        g_ci_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void ci_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        kos_sleep_ns(g_call_unit * 3);         // ready before recv#2; blocked while the server is boosted
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void ci_filler(void*) // caps: done@1, lock@2, E(SIGNAL)@3 (a plain sender)
    {
        char b[4] = {0};
        kos_send(3, b, 4);                     // plain send: parks, consumed by one of the server's recvs
        kos_sem_post(CH_DONE);
    }
    void t_call_infoless_revert()
    {
        // Ask the pool BEFORE spawning anything. These four workers are mutually
        // dependent -- the server parks in recv for the filler's send, the caller's
        // boost is what the spoiler races -- so a partial set cannot be drained: the
        // ones that started wait on posts the missing ones would have made. Guarding
        // after the spawns, as this test used to, therefore does not skip on a small
        // board, it HANGS, and that is what took the microbit gate down from M4.4
        // (9ae301f) to M4.5.1.
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (4 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        g_ci_rc = -99;
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int sv = kos::thread::spawn_caps(ci_server, nullptr, "ciS", 8, scaps, 3);
        int cl = kos::thread::spawn_caps(ci_caller, nullptr, "ciC", 20, ccaps, 3);
        int sp = kos::thread::spawn_caps(ci_spoiler, nullptr, "ciM", 12, mcaps, 2);
        int fl = kos::thread::spawn_caps(ci_filler, nullptr, "ciF", 6, ccaps, 3);
        // The probe above just held four slots and four stacks, and nothing else runs
        // between it and here, so a failure now is a pool bug, not a small board.
        TAP_CHECK(sv >= 0 and cl >= 0 and sp >= 0 and fl >= 0);
        char warm[4] = {0};
        kos_send(g_ep, warm, 4); // second plain sender; recv#1 eats the filler, recv#2 eats this
        wait_n(4);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_ci_rc == -KOS_ENOSYS); // the call bounced off the info-less receiver
        TAP_CHECK(count('a') == 1 and count('u') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST held: boosted server outran the spoiler's wake
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: server back at base, spoiler ran before it resumed
    }

    // --- Call/reply (M4.4): close-instead-of-reply EPIPEs the caller AND yields to it -----
    // A low server takes a high caller's call (D1-boosted to the caller's prio), then closes
    // the reply cap instead of replying. The close arm must (a) wake the caller -KOS_EPIPE
    // and (b) deflate the server BEFORE waking, so the higher caller runs before the server
    // proceeds -- 'c' strictly before 's'.
    volatile long g_cc_rc = -99;
    void cc_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, -1};
        kos_recv(3, buf, sizeof(buf), &info); // info-bearing: hosts the call, D1-boosts us, mints a reply cap
        kos_handle_close(info.reply_cap);     // close instead of reply: EPIPE the caller + deflate us
        log_put('s');                         // server proceeds -- must be AFTER the caller ran
        kos_sem_post(CH_DONE);
    }
    void cc_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(3000000ull);             // let the server park in recv (fast-path call)
        g_cc_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void t_call_close_reply()
    {
        log_reset();
        g_cc_rc = -99;
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        int sv = kos::thread::spawn_caps(cc_server, nullptr, "ccS", 8, scaps, 3);
        int cl = kos::thread::spawn_caps(cc_caller, nullptr, "ccC", 20, ccaps, 3);
        if (sv < 0 or cl < 0)
        {
            int n = 0;
            if (sv >= 0) { n++; }
            if (cl >= 0) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_cc_rc == -KOS_EPIPE);   // (a) caller woken with EPIPE, not a byte count
        TAP_CHECK(nth('c', 1) < nth('s', 1)); // (b) caller ran before the server proceeded
    }

    // --- Call/reply (M4.4): a NON-pool caller is rejected, never faults -----------------
    // The selftest orchestrator runs on the file-static root/init TCB, which is NOT a
    // ThreadPool slot, so it cannot be named by a reply cap. kos_call from here must fail
    // -KOS_EPERM up front. This genuinely exercises the guard: a POOL caller with a valid
    // SIGNAL cap and no parked receiver would slow-path PARK (hang), so an immediate
    // -KOS_EPERM proves the non-pool check fired, not a rights or park path.
    void t_call_nonpool_caller()
    {
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        char buf[8] = {0};
        TAP_CHECK(kos_call(g_ep, buf, 4, sizeof(buf)) == -KOS_EPERM);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Call/reply (M4.4): happy path -- request delivered, reply returned in-place ---
    // A server recvs the request (info-bearing), records it, and replies a known payload;
    // the caller's kos_call returns the reply byte count and the reply OVERWRITES its send
    // buffer (in-place). Both paths are covered: (A) server parked in recv first (fastpath),
    // (B) caller parked in SEND_WAIT first, server recvs later (slowpath).
    volatile long g_echo_reqn = -99; // request bytes the server observed
    char g_echo_reqbuf[8];           // request content the server observed
    volatile long g_echo_rc = -99;   // caller's kos_call return
    char g_echo_rplbuf[8];           // reply content the caller received in-place
    void echo_server(void*)          // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        long n = kos_recv(2, buf, sizeof(buf), &info);
        g_echo_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_echo_reqbuf)) { k = sizeof(g_echo_reqbuf); }
            memcpy(g_echo_reqbuf, buf, k);
        }
        if (info.reply_cap >= 0)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5); // reply from the server's OWN stack (unpriv-readable)
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void echo_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[16];
        memcpy(buf, "ping", 4);
        g_echo_rc = kos_call(2, buf, 4, sizeof(buf)); // reply lands back in buf
        if (g_echo_rc > 0)
        {
            size_t k = static_cast<size_t>(g_echo_rc);
            if (k > sizeof(g_echo_rplbuf)) { k = sizeof(g_echo_rplbuf); }
            memcpy(g_echo_rplbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    void t_call_happy()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};   // done@1, E(WAIT)@2
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2

        // (A) fastpath: server parks in recv first, then the caller calls.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        int sv = kos::thread::spawn_caps(echo_server, nullptr, "echS", 10, scaps, 2);
        int cl = -1;
        if (sv >= 0)
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath)
            cl = kos::thread::spawn_caps(echo_caller, nullptr, "echC", 12, ccaps, 2);
        }
        if (sv < 0 or cl < 0)
        {
            // cl is spawned only after sv succeeds, so a skip means either nothing spawned
            // (sv<0) or a lone server parked in recv (cl<0). Neither can be drained: close
            // and skip. Never fires on the CI targets (>= 2 thread slots).
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_echo_reqn == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0); // request delivered
        TAP_CHECK(g_echo_rc == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);  // reply back in-place

        // (B) slowpath: caller parks in SEND_WAIT first, server recvs later.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        int cl2 = kos::thread::spawn_caps(echo_caller, nullptr, "echC2", 12, ccaps, 2);
        int sv2 = -1;
        if (cl2 >= 0)
        {
            kos_sleep_ns(3000000ull); // let the caller park in SEND_WAIT (slowpath)
            sv2 = kos::thread::spawn_caps(echo_server, nullptr, "echS2", 10, scaps, 2);
        }
        if (cl2 < 0 or sv2 < 0)
        {
            // The caller (spawned first) may be parked in SEND_WAIT: close FIRST so it is
            // EPIPE'd and posts, THEN drain it. Never fires on the CI targets.
            kos_handle_close(g_ep);
            if (cl2 >= 0) { wait_n(1); }
            tap::diag("call_happy: PARTIAL -- slowpath half not run (pool too small)");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_echo_reqn == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(g_echo_rc == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);
    }

    // --- Call/reply (M4.4): reply + request truncation (datagram clamp, not an error) ---
    // One call exercises BOTH clamps: the caller sends 8 bytes into a server recv buffer of
    // 3 (request truncated to 3), and the server replies 8 bytes into a caller recv_cap of 3
    // (reply truncated to 3). Neither is an error -- the byte counts just clamp.
    volatile long g_trunc_reqn = -99; // request bytes the server saw (its buffer < send_len)
    char g_trunc_reqbuf[4];
    volatile long g_trunc_rc = -99; // caller's kos_call return (clamped to recv_cap)
    char g_trunc_rplbuf[4];
    void trunc_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[3]; // smaller than the 8-byte request
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        long n = kos_recv(2, buf, sizeof(buf), &info); // request clamps to 3
        g_trunc_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_trunc_reqbuf)) { k = sizeof(g_trunc_reqbuf); }
            memcpy(g_trunc_reqbuf, buf, k);
        }
        if (info.reply_cap >= 0)
        {
            char rpl[8];
            memcpy(rpl, "12345678", 8);
            kos_reply(info.reply_cap, rpl, 8); // 8 offered, caller cap is 3 -> clamps to 3
        }
        kos_sem_post(CH_DONE);
    }
    void trunc_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8];
        memcpy(buf, "ABCDEFGH", 8);
        g_trunc_rc = kos_call(2, buf, 8, 3); // recv_cap = 3 -> the reply clamps into it
        if (g_trunc_rc > 0)
        {
            size_t k = static_cast<size_t>(g_trunc_rc);
            if (k > sizeof(g_trunc_rplbuf)) { k = sizeof(g_trunc_rplbuf); }
            memcpy(g_trunc_rplbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    void t_call_truncation()
    {
        g_trunc_reqn = -99; g_trunc_rc = -99;
        memset(g_trunc_reqbuf, 0, sizeof(g_trunc_reqbuf));
        memset(g_trunc_rplbuf, 0, sizeof(g_trunc_rplbuf));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        int sv = kos::thread::spawn_caps(trunc_server, nullptr, "trS", 10, scaps, 2);
        int cl = -1;
        if (sv >= 0)
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::spawn_caps(trunc_caller, nullptr, "trC", 12, ccaps, 2);
        }
        if (sv < 0 or cl < 0)
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_trunc_reqn == 3 and memcmp(g_trunc_reqbuf, "ABC", 3) == 0); // request clamped
        TAP_CHECK(g_trunc_rc == 3 and memcmp(g_trunc_rplbuf, "123", 3) == 0);   // reply clamped
    }

    // --- Call/reply (M4.4): a second reply on a consumed cap is rejected -----------------
    // The reply cap is one-shot: the first kos_reply consumes it (empty slot + gen bump), so
    // a second kos_reply on the same handle fails resolve with -KOS_EBADF.
    volatile int g_dr_second = -99; // second kos_reply rc
    volatile long g_dr_callrc = -99;
    void dr_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        kos_recv(2, buf, sizeof(buf), &info);
        if (info.reply_cap >= 0)
        {
            char rpl[4];
            memcpy(rpl, "ok", 2);
            kos_reply(info.reply_cap, rpl, 2);                  // consumes the cap
            g_dr_second = kos_reply(info.reply_cap, rpl, 2);    // cap gone -> -KOS_EBADF
        }
        kos_sem_post(CH_DONE);
    }
    void dr_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_dr_callrc = kos_call(2, buf, 4, sizeof(buf));
        kos_sem_post(CH_DONE);
    }
    void t_call_double_reply()
    {
        g_dr_second = -99; g_dr_callrc = -99;
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        int sv = kos::thread::spawn_caps(dr_server, nullptr, "drS", 10, scaps, 2);
        int cl = -1;
        if (sv >= 0)
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::spawn_caps(dr_caller, nullptr, "drC", 12, ccaps, 2);
        }
        if (sv < 0 or cl < 0)
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_dr_callrc == 2);            // caller got the (single) reply
        TAP_CHECK(g_dr_second == -KOS_EBADF);   // the second reply was rejected
    }

    // --- Call/reply (M4.4): server dies mid-transaction -> caller EPIPE (teardown arm) ---
    // The server takes the call (REPLY_WAIT, holding the reply cap) then exits WITHOUT
    // replying. cap_teardown walks its table, hits the CAP_REPLY arm, and wakes the parked
    // caller with -KOS_EPIPE.
    volatile long g_sd_callrc = -99;
    void sd_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        kos_recv(2, buf, sizeof(buf), &info); // takes the call, holds a reply cap
        kos_sem_post(CH_DONE);                // report BEFORE exiting owning the cap
        kos_exit(0);                          // teardown EPIPEs the parked caller
    }
    void sd_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_sd_callrc = kos_call(2, buf, 4, sizeof(buf)); // woken -KOS_EPIPE when the server dies
        kos_sem_post(CH_DONE);
    }
    void t_call_server_death()
    {
        g_sd_callrc = -99;
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        int sv = kos::thread::spawn_caps(sd_server, nullptr, "sdS", 10, scaps, 2);
        int cl = -1;
        if (sv >= 0)
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath call)
            cl = kos::thread::spawn_caps(sd_caller, nullptr, "sdC", 12, ccaps, 2);
        }
        if (sv < 0 or cl < 0)
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0); // server's WAIT cap already gone -> main's is the last
        TAP_CHECK(g_sd_callrc == -KOS_EPIPE);   // caller woken EPIPE by the reply-cap teardown
    }

    // --- Call/reply (M4.4): server dies pre-pop -> caller EPIPE (recv_holders -> 0) ------
    // The caller parks in SEND_WAIT (no receiver has popped it yet). MAIN holds the sole
    // WAIT cap; closing it drives recv_holders to 0, which drains send_waiters and EPIPEs
    // the parked call -- the pre-pop counterpart to the mid-transaction teardown above.
    volatile long g_pp_callrc = -99;
    void pp_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_pp_callrc = kos_call(2, buf, 4, sizeof(buf)); // parks SEND_WAIT; woken -KOS_EPIPE
        kos_sem_post(CH_DONE);
    }
    void t_call_prepop_death()
    {
        g_pp_callrc = -99;
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        // Caller gets SIGNAL only; MAIN is the sole WAIT holder. No server ever recvs.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        int cl = kos::thread::spawn_caps(pp_caller, nullptr, "ppC", 12, ccaps, 2);
        TAP_CHECK(cl >= 0); // spawn failure would hang the drain below
        kos_sleep_ns(3000000ull);               // let the caller park in SEND_WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);  // last WAIT cap -> recv_holders 0 -> EPIPE the call
        wait_n(1);
        TAP_CHECK(g_pp_callrc == -KOS_EPIPE);
    }

    // --- Call/reply (M4.4): donation ordering (positive) --------------------------------
    // low(8) server, high(20) caller, medium(12) spoiler. On the fastpath call the server is
    // D1-boosted to the caller's prio, so the spoiler -- which wakes WHILE the server holds
    // the transaction -- cannot preempt: the reply reaches the caller ('c') before the
    // spoiler runs ('m'). The positive counterpart to call_infoless_revert. Without donation
    // the medium spoiler would preempt the low server mid-transaction and 'm' would precede 'c'.
    uint64_t g_don_unit = 1000000ull;
    volatile long g_don_rc = -99;
    char g_don_rpl[8];
    void don_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        kos_recv(3, buf, sizeof(buf), &info); // parks first (no senders); D1-boosted at the call
        log_put('a');
        mtx_spin(g_don_unit * 4); // hold the CPU past the spoiler's wake, at the boosted prio
        log_put('r');
        if (info.reply_cap >= 0)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5); // deflate + wake the caller (it preempts now)
        }
        kos_sem_post(CH_DONE);
    }
    void don_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call after the server has parked in recv (fastpath)
        memcpy(buf, "req", 3);
        g_don_rc = kos_call(3, buf, 3, sizeof(buf));
        if (g_don_rc > 0)
        {
            size_t k = static_cast<size_t>(g_don_rc);
            if (k > sizeof(g_don_rpl)) { k = sizeof(g_don_rpl); }
            memcpy(g_don_rpl, buf, k);
        }
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void don_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        kos_sleep_ns(g_don_unit * 3); // wake while the server holds the boosted transaction
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation()
    {
        log_reset();
        g_don_unit = mtx_time_unit();
        g_don_rc = -99;
        memset(g_don_rpl, 0, sizeof(g_don_rpl));
        g_ep = kos_endpoint_create();
        TAP_CHECK(g_ep >= 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        int sv = kos::thread::spawn_caps(don_server, nullptr, "dnS", 8, scaps, 3);
        int cl = kos::thread::spawn_caps(don_caller, nullptr, "dnC", 20, ccaps, 3);
        int sp = kos::thread::spawn_caps(don_spoiler, nullptr, "dnM", 12, mcaps, 2);
        if (sv < 0 or cl < 0 or sp < 0)
        {
            // Drain whoever spawned (each posts g_done: the caller drives the server through
            // its reply, the spoiler is timed), close the endpoint, skip. Mirrors ci_infoless.
            int n = 0;
            if (sv >= 0) { n++; }
            if (cl >= 0) { n++; }
            if (sp >= 0) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('c') == 1 and count('m') == 1);
        TAP_CHECK(g_don_rc == 5 and memcmp(g_don_rpl, "pong!", 5) == 0); // the transaction completed
        TAP_CHECK(nth('r', 1) < nth('c', 1)); // reply delivered: the caller ran after the server replied
        TAP_CHECK(nth('c', 1) < nth('m', 1)); // DONATION: the reply reached the caller before the spoiler ran
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
            g_ep_badrecv_rc = kos_recv(2, bad, 8, nullptr);           // write oracle -> -KOS_EFAULT
            g_ep_badsend_rc = kos_send(2, static_cast<char const*>(bad), 8); // cross-domain read -> -KOS_EFAULT
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
            TAP_CHECK(g_ep_badrecv_rc == -KOS_EFAULT); // bad recv buffer rejected, never parked
            TAP_CHECK(g_ep_badsend_rc == -KOS_EFAULT); // bad send buffer rejected, never parked
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
            tap::skip("arena cannot spare two domain regions");
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
            tap::skip("thread pool too small for 2 concurrent");
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
        // KCAP_INDEX_BITS == 4). cap_install scans from KOS_CAP_FIRST_DYNAMIC, so an own
        // sem/endpoint/mutex create never returns a reserved well-known slot (0 = console
        // default; 1..FIRST_DYNAMIC-1 = board/service delegation) -- it lands at
        // >= FIRST_DYNAMIC. This is the FROZEN cap-index convention (cap_index.h) enforced
        // kernel-side, so a board that delegates no well-known cap cannot let the app's
        // first create alias a reserved index.
        //
        // FIRST_DYNAMIC-floor TRIPWIRE: the `>= KOS_CAP_FIRST_DYNAMIC` checks below fail
        // LOUDLY if cap_install ever regresses to scanning from 1 (own creates would then
        // land at index 1..3, aliasing the reserved range). Delegation uses explicit
        // indices, so it is blind to the scan floor -- only an OWN create catches it.
        constexpr int IDX_MASK = 0xF;
        int s = kos_sem_create(0);
        TAP_CHECK(s >= 0 and (s & IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        int e = kos_endpoint_create();
        TAP_CHECK(e >= 0 and (e & IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        int m = kos_mutex_create();
        TAP_CHECK(m >= 0 and (m & IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        TAP_CHECK(kos_handle_close(s) == 0);
        TAP_CHECK(kos_handle_close(e) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);

        // Index 0 is the kernel stdout slot, and BOTH of its postures are asserted here --
        // this test used to hardcode the pre-publish one, which made it fail on every
        // board whose service list publishes the console (and, before the harness became
        // publish-aware, its `not ok` was itself swallowed).
        //
        // The discriminator is a ZERO-length send: a valid zero-length signal per
        // <kickos/sys.h>, so unlike the 1-byte probe below it puts NO byte on the wire in
        // either posture -- the old unconditional "x" is what left a stray character in
        // front of the published TAP stream.
        long const stdout_seated = kos_send(0, "", 0);
        if (stdout_seated == -KOS_EBADF)
        {
            // Pre-publish (g_stdout_target < 0): cap_install_defaults seats NOTHING at
            // index 0, so a send fails cleanly rather than resolving a stale/aliased
            // object. Exercises the pre-publish cap_install_defaults branch.
            TAP_CHECK(kos_send(0, "x", 1) == -KOS_EBADF);
        }
        else
        {
            // Post-publish: cap_seat_stdout put a send-only (CAP_SIGNAL) endpoint cap at
            // index 0, so the zero-length signal rendezvoused with the console driver and
            // reported 0 bytes transferred. Exercises the other cap_install_defaults
            // branch -- and, because a rendezvous only completes with a live receiver, it
            // is the one place the suite proves console output is actually being ACKed.
            TAP_CHECK(stdout_seated == 0);
        }

        // Exhaustion: own-creates fill the remaining slots [FIRST_DYNAMIC .. MAX_HANDLES-1]
        // and then fail cleanly with -KOS_ENOMEM -- the reserved range stays off-limits even at the
        // LAST free slot, and a full table never crashes or returns a reserved index. (Index
        // field is 4 bits: MAX_HANDLES <= 16.)
        int held[16];
        int n = 0;
        while (true)
        {
            int h = kos_sem_create(0);
            if (h < 0)
            {
                break;
            }
            TAP_CHECK((h & IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC); // never a reserved slot, not even the last free one
            held[n] = h;
            n = n + 1;
            if (n >= static_cast<int>(sizeof(held) / sizeof(held[0])))
            {
                break;
            }
        }
        TAP_CHECK(n >= 1);
        TAP_CHECK(kos_sem_create(0) == -KOS_ENOMEM); // table full -> clean exhaustion code
        TAP_CHECK(kos_sem_create(0) == -KOS_ENOMEM); // still ENOMEM (idempotent, no side effect)
        for (int i = 0; i < n; i++)
        {
            TAP_CHECK(kos_handle_close(held[i]) == 0);
        }
        int again = kos_sem_create(0); // table recovers once slots are freed
        TAP_CHECK(again >= 0 and (again & IDX_MASK) != 0);
        TAP_CHECK(kos_handle_close(again) == 0);
    }

    // --- console_publish is privileged-only; a bad cap is rejected with no side effect --
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console -- the rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1); // unprivileged -> -KOS_EPERM
        kos_sem_post(CH_DONE);
    }
    void t_console_publish()
    {
        // Privileged MAIN: a bad/stale cap is rejected before the deinit/flip, so console
        // ownership is left exactly as the board's service list set it -- whichever posture
        // that is. (This test never publishes anything itself: both caps below are invalid,
        // and the unprivileged child is refused by the privilege gate. A board that DID
        // publish keeps its driver, and the harness follows it there -- see tests/tap/tap.cc.)
        TAP_CHECK(kos_console_publish(-1) == -KOS_EBADF);
        TAP_CHECK(kos_console_publish(0x7fffffff) == -KOS_EBADF);
        // Unprivileged child: the privileged-only gate rejects it.
        g_pub_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int w = kos::thread::spawn_caps(pub_denied_worker, nullptr, "pubDen", 10, caps, 1);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_pub_rc == -KOS_EPERM); // unprivileged console_publish refused
    }

    // --- shutdown is privileged-only: an unprivileged thread cannot end the system ----
    int g_shutdown_rc = -99;
    void shutdown_denied_worker(void*) // caps: done@1
    {
        // Status 0 on purpose. If this gate ever regresses, the run ENDS HERE, mid-suite,
        // with a clean exit status -- so passing 0 is what makes the regression look like
        // a truncated TAP stream rather than a successful one.
        g_shutdown_rc = kos_shutdown(0);
        kos_sem_post(CH_DONE);
    }
    void t_shutdown_denied()
    {
        g_shutdown_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int w = kos::thread::spawn_caps(shutdown_denied_worker, nullptr, "sdDen", 10, caps, 1);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_shutdown_rc == -KOS_EPERM); // unprivileged shutdown refused
    }

    // --- a syscall buffer that lives in an app GLOBAL, from an unprivileged thread ----
    // An unprivileged thread's writable set is [app static data] + its domain + its own
    // stack. On any backend that does not model app static data as an MPU region -- every
    // no-MPU chip, and the host sim, whose globals sit in the host image rather than the
    // mprotect'd arena -- that collapses to the stack alone, so a syscall buffer in a
    // global is refused EFAULT even though the thread can plainly store there itself.
    //
    // recv validates its buffer BEFORE resolving the cap, so a deliberately invalid cap
    // separates the two answers with no rendezvous and no sender: EBADF means the buffer
    // was admitted and we got as far as the cap, EFAULT means it was not.
    char g_wrbuf[16];
    long g_wrbuf_rc = -99;
    void wrbuf_worker(void*) // caps: done@1
    {
        g_wrbuf_rc = kos_recv(0x7fffffff, g_wrbuf, sizeof(g_wrbuf), nullptr);
        kos_sem_post(CH_DONE);
    }
    void t_writable_global()
    {
        g_wrbuf_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int w = kos::thread::spawn_caps(wrbuf_worker, nullptr, "wrGlob", 10, caps, 1);
        TAP_CHECK(w >= 0);
        wait_n(1);
        TAP_CHECK(g_wrbuf_rc == -KOS_EBADF); // not -KOS_EFAULT: the global was writable
    }

    // --- The authority capability: the non-privileged arm of the eight gates ----------
    // Each authority gate is `privileged OR holds this AUTH_* bit`. Root is privileged, so
    // the whole suite already leans on the privileged arm; this covers the OTHER one, which
    // would otherwise ship unexercised until stage 2 flips a board -- the same vacuity trap
    // the ctor-placement gate fell into.
    //
    // The child is UNPRIVILEGED and holds AUTH_PINMUX and nothing else, so exactly one gate
    // must accept it and the rest must refuse. Acceptance reads as "not -KOS_EPERM", because
    // a gate that lets the call through returns its OWN answer instead -- -KOS_ENOSYS on a
    // weak-seam target like the sim, -KOS_EINVAL where a chip owns the block -- and that
    // distinction is exactly what a privilege refusal erases.
    void auth_noop(void*) {}
    volatile long g_auth_pinmux = -99;   // AUTH_PINMUX held    -> anything but -KOS_EPERM
    volatile long g_auth_shutdown = -99; // AUTH_DEVICE absent  -> -KOS_EPERM
    volatile long g_auth_regrant = -99;  // may not hand on a bit it does not hold
    volatile long g_auth_collide = -99;  // delegation packing reaching the authority slot
    volatile long g_auth_badbits = -99;  // object rights offered as an authority
    volatile long g_auth_capsarr = -99;  // the grant ARRAY read, reached past the early refusals
    kos_thread_params g_auth_kid;  // deliberately static: see auth_worker (N15)
    kos_cap_grant g_auth_two[2];   // ditto -- the OTHER read thread_spawn validates
    void auth_worker(void*) // UNPRIVILEGED, authority = AUTH_PINMUX; caps: done@1
    {
        // The bit it HOLDS: past the gate, so pinmux answers for itself.
        g_auth_pinmux = kos_pinmux_set(99u, 0u, 0x10u);
        // A bit it does NOT hold, at a different gate -- so this also proves the bits are
        // independent rather than one lump. shutdown is the highest-stakes of the four and
        // it stands for all of them; console_publish_priv and shutdown_priv already cover
        // the privilege dimension of the same gates. Safe to call precisely BECAUSE the
        // child lacks AUTH_DEVICE: a regression ends the run here with a clean status,
        // which the harness sees as a truncated TAP stream.
        g_auth_shutdown = kos_shutdown(0);
        // Three spawn probes off ONE params struct, all refused before a pool slot is
        // claimed, so their codes are deterministic even on a full pool. One struct is not
        // tidiness: a frame-local kos_thread_params costs an inline zero-init per site, and
        // this suite links within ~100 bytes of the f302nucleo flash ceiling.
        //
        // g_auth_kid and g_auth_two are GLOBALS on purpose, and that is the whole of this
        // test's N15 coverage: thread_spawn reads the params struct and the grant array
        // through user_readable_ok, so a caller may keep either in static data. Before that
        // fix both reads were a raw user_range_ok with no arch_user_text_readable arm, and
        // all three probes below answered -KOS_EFAULT on the sim and on every no-MPU board
        // instead of the codes they assert.
        kos_thread_params& kid = g_auth_kid;
        kid.entry = auth_noop;
        kid.prio = 9;
        // Narrow-only, the same rule a cap_grant mask obeys: holding AUTH_PINMUX does not
        // let it seat AUTH_DEVICE on a child.
        kid.authority = KOS_AUTH_DEVICE;
        g_auth_regrant = kos_thread_spawn(&kid);
        // Delegated cap i lands at child index i+1, so two delegated caps reach the
        // authority slot. Refused, rather than one silently overwriting the other.
        g_auth_two[0] = {CH_DONE, CH_FULL};
        g_auth_two[1] = {CH_DONE, CH_FULL};
        kid.caps = g_auth_two;
        kid.cap_count = 2;
        kid.authority = KOS_AUTH_PINMUX;
        g_auth_collide = kos_thread_spawn(&kid);
        // Object rights mean nothing on this type, so they are refused, not masked off.
        kid.cap_count = 1;
        kid.authority = KOS_CAP_WAIT;
        g_auth_badbits = kos_thread_spawn(&kid);
        // All three probes above are refused by an authority check that runs BEFORE the
        // delegation loop, so none of them reads g_auth_two at all -- the array is the
        // second N15 site and needs a probe that gets that far. An unresolvable
        // source_cap is refused -KOS_EBADF from inside the loop, which is only reachable
        // once the array itself has been admitted; with the raw range check it answers
        // -KOS_EFAULT instead. Refused before a pool slot is claimed, like the others.
        g_auth_two[0] = {0x7fffffff, CH_FULL};
        kid.authority = 0;
        g_auth_capsarr = kos_thread_spawn(&kid);
        kos_sem_post(CH_DONE);
    }
    void t_authority_cap()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int w = kos::thread::spawn_caps(auth_worker, nullptr, "authW", 10, caps, 1,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        nullptr, 0, /*authority=*/KOS_AUTH_PINMUX);
        if (w < 0)
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // Grouped, because each TAP_CHECK carries __FILE__ and its stringified condition as
        // rodata and this image is at the f302nucleo ceiling. Held bit accepted (pinmux
        // returned its own answer: -KOS_ENOSYS on a weak seam, -KOS_EINVAL where a chip owns
        // the block), unheld bit refused at another gate, and the three spawn refusals.
        TAP_CHECK(g_auth_pinmux != -KOS_EPERM and g_auth_pinmux < 0);
        TAP_CHECK(g_auth_shutdown == -KOS_EPERM);
        TAP_CHECK(g_auth_regrant == -KOS_EPERM and g_auth_collide == -KOS_EINVAL
                  and g_auth_badbits == -KOS_EINVAL and g_auth_capsarr == -KOS_EBADF);
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
    tap::add("periph_clock_hz", t_periph_clock_hz);
    tap::add("pinmux_set", t_pinmux_set);
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
    tap::add("mutex_unlock_errors", t_mutex_unlock_errors); // non-owner / unlocked -> -KOS_EPERM
    tap::add("mutex_owner_died_nowaiter", t_mutex_owner_died_nowaiter); // R3 no-waiter branch
    tap::add("mutex_deleg_refcount", t_mutex_deleg_refcount); // child close, parent still locks
    // Endpoint IPC (M3 #4 stage i): production syscalls, so runs on every board.
    tap::add("endpoint_rendezvous", t_endpoint_rendezvous); // both orderings + zero-len + truncation
    tap::add("endpoint_reject", t_endpoint_reject);         // F4 oversize + bad cap
    tap::add("endpoint_rights", t_endpoint_rights);         // send needs SIGNAL, recv needs WAIT
    tap::add("endpoint_epipe", t_endpoint_epipe);           // parked sender woken on last WAIT close
    tap::add("endpoint_dead", t_endpoint_dead);             // F1 dead endpoint: send refused, no park
    tap::add("call_infoless_revert", t_call_infoless_revert); // M4.4: info-less bounce reverts the D2 boost
    tap::add("call_close_reply", t_call_close_reply);         // M4.4: close-instead-of-reply EPIPEs + yields
    tap::add("call_nonpool_caller", t_call_nonpool_caller);   // M4.4: non-pool (root) caller rejected, no fault
    tap::add("call_happy", t_call_happy);                     // M4.4: request delivered + reply in-place (fast+slow)
    tap::add("call_truncation", t_call_truncation);           // M4.4: request + reply datagram clamp
    tap::add("call_double_reply", t_call_double_reply);       // M4.4: one-shot cap -> second reply -KOS_EBADF
    tap::add("call_server_death", t_call_server_death);       // M4.4: die mid-xact -> caller EPIPE (teardown arm)
    tap::add("call_prepop_death", t_call_prepop_death);       // M4.4: die pre-pop -> caller EPIPE (recv_holders 0)
    tap::add("call_donation", t_call_donation);               // M4.4: D1 donation keeps the spoiler off the xact
    tap::add("endpoint_crossdomain", t_endpoint_crossdomain); // F5 cross-domain copy + delegation
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    tap::add("endpoint_bound", t_endpoint_bound); // bound-check: bad recv/send buffer refused
#endif
    // Console handover mechanism (M3 #4 stage ii-a): production syscalls, every board.
    tap::add("cap_index0", t_cap_index0);              // B3 index-0 reservation + FIRST_DYNAMIC floor
    tap::add("console_publish_priv", t_console_publish); // D3 privileged-only + bad-cap reject
    tap::add("shutdown_priv", t_shutdown_denied);        // KOS_SYS_SHUTDOWN privileged-only
    tap::add("writable_global", t_writable_global);      // out-buffer in an app global
    tap::add("authority_cap", t_authority_cap);          // CAP_AUTHORITY: both arms of the gates
#if defined(KICKOS_ENABLE_SELFTEST)
    // Need the software-inject syscall (compiled out of the production ABI).
    tap::add("irq_thread_ctx", t_irq);
    tap::add("irq_as_event", t_irqdrv);
    tap::add("irq_mask_coalesce", t_irq_mask);
    tap::add("irq_autorearm", t_irq_autorearm);
    tap::add("irq_phantom_wake", t_irq_phantom);
    tap::add("irq_ownership", t_irq_ownership);
    tap::add("irq_spurious", t_irq_spurious);
    tap::add("irq_stale_register", t_irq_stale_register);
#if KICKOS_HAVE_MPU
    tap::add("mpu_privileged_guard", t_mpu_guard); // needs enforced protection
#endif
#endif
    tap::add("caller_stack", t_caller_stack); // caller-owned stack API (no test-only syscalls)
    tap::add("domain_share", t_domain_share); // two threads share one memory domain
    tap::add("mmio_grant", t_mmio_grant);     // MMIO-grant boundary: privileged-only + encodable-only
#if KICKOS_HAVE_MPU
    tap::add("stackbase_arena", t_stackbase_arena); // unprivileged out-of-arena stack_base refused
#if defined(KICKOS_ENABLE_SELFTEST)
    tap::add("grant_reserved", t_grant_reserved);   // Rule 7: overlap matrix + RAM/DEV admission (probe syscall)
#endif
#endif
    tap::add("confused_deputy", t_confused_deputy); // readable-buffer/name floor (accept rodata, reject bogus)

    // Every test joins its workers, so main returns as the last live thread:
    // the failure count becomes the process exit status (0 == all passed).
    return tap::run_all();
}
