// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS self-test (unprivileged userspace, C++), the CI gate: every verification
// bullet is a TAP arm that self-asserts its invariant over the console (tests/tap).
// Ordering-sensitive arms assert on a semaphore-locked event log, never on console text.
//
// The deliberate cross-domain MPU fault is a separate binary (apps/mpu_fault): it ends
// the process, so it cannot be an arm here.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/config/cap_width.h>
#include <kickos/sys/bus.h> // compile-checks the wire-ABI struct-size static_asserts
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/uart_service.h>
#include <kickos/sys/spi_service.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/string.h>

#include <atomic>

#include "tap.h"

// The chip's own constants. A sim build ships none (same guard as config/board.h), so
// anything read from here needs a fallback.
#if defined(__has_include) and __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// Start of the IRQ arms' block of nine consecutive lines. They need lines no other holder
// in this image claims, which is weaker than KICKOS_IRQ_SOFT_ONLY_BASE's no-source
// property. A chip whose own drivers sit in the default block must move it.
#ifndef KICKOS_SELFTEST_IRQ_BASE
#define KICKOS_SELFTEST_IRQ_BASE 6
#endif

// Which region of the registration list at the bottom of this file to register: 0 (the
// default) is all of it, 1 and 2 are the two contiguous regions the 64 KiB FLASH parts
// build as separate images. TAP_ADD is REDEFINED at the region boundary, so an arm
// belongs to the part its line sits in; it cannot be annotated into the other one.
#ifndef KICKOS_SELFTEST_PART
#define KICKOS_SELFTEST_PART 0
#endif

// Unevaluated operand: the arm still counts as used for -Wunused-function, and nothing
// references it, so an elided body is never emitted.
#define TAP_ELIDE(fn) ((void)sizeof(&(fn)))

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    kos_cap_t g_done = KOS_CAP_NONE; // shared completion counter (MAIN's cap; delegated to workers)
    kos_cap_t g_lock = KOS_CAP_NONE; // binary semaphore = mutex over the event log (MAIN's cap)

    // Well-known child cap indices. A fresh child table has cap-gen 0, so a delegated
    // cap's handle value == its table index, and delegated cap i lands at index i+1
    // (index 0 reserved). The worker helpers below name caps by these constants, so every
    // spawn must delegate in exactly this order.
    constexpr int CH_DONE = 1;  // delegated FIRST to every worker
    constexpr int CH_LOCK = 2;  // delegated SECOND (logging workers only)
    constexpr int CH_AUX = 3;
    constexpr int CH_READY = 2; // IRQ-driver tests: done@1, ready@2
    constexpr int CH_IRQ = 3;   // IRQ-driver tests: line@3
    constexpr uint8_t CH_FULL =
        KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // root's region set is [app code RX, app static data RW, its own stack]: it cannot
    // spawn a privileged child, and kos_ram_alloc grants the caller nothing. A test that
    // must touch its own allocation asks with kos_mem_self_grant, as t_irqdrv does.

    char g_log[128];
    int g_logn = 0;

    void log_reset()
    {
        g_logn = 0;
        g_log[0] = 0;
    }

    // Worker threads only: CH_LOCK is a child-table index, meaningless in root.
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

    // g_done outlives every arm and there is no sem_trywait to drain it with, so a fresh
    // object is the only way back to a known count: an arm that a failing TAP_CHECK
    // returned out of leaves the posts of its remaining wait_n calls banked, and the next
    // arm's first wait_n takes one of those instead of its own event, reporting one real
    // failure as several. A worker still running from the abandoned arm holds a cap to the
    // OLD object, whose bumped generation refuses its post.
    // ONLY after a failure: between every arm it churns a cap slot 90 times over and starves
    // the later spawns on the smallest board.
    void done_reset()
    {
        kos_sem_destroy(g_done);
        kos_sem_create(0, &g_done);
    }

    // Staging gate: every worker of a test must exist before ANY of them runs. A spawn
    // does not itself reschedule, but an interrupt landing between two spawns does, and a
    // worker created above root's priority then runs early.
    // The gate MUST be its own semaphore: gating on the event-log mutex hands the token
    // straight to the next waiter and the workers ping-pong through log_put instead.
    // stage_wait must be the worker's first statement; root posts once after the last
    // spawn and each worker re-posts for the next.
    kos_cap_t g_gate = KOS_CAP_NONE;
    void stage_release()
    {
        kos_sem_post(g_gate);
    }
    void stage_wait(kos_cap_t gate)
    {
        kos_sem_wait(gate);
        kos_sem_post(gate);
    }

    char arg_char(void* arg)
    {
        return static_cast<char>(reinterpret_cast<uintptr_t>(arg));
    }

    // The arena's allocation granule: arch_ram_region_size rounds 1 up to exactly one
    // granule in every encoding mode, so two consecutive one-byte blocks sit one granule
    // apart. Returns 0 when the arena cannot host both. Memoised: kos_ram_alloc never
    // frees, and on a 16 KiB part mem_self_grant needs that arena to reach the
    // region-descriptor ceiling.
    size_t g_granule = 0;

    size_t discover_granule()
    {
        if (g_granule != 0)
        {
            return g_granule;
        }
        void* p = kos_ram_alloc(1);
        void* q = kos_ram_alloc(1);
        if (p == nullptr or q == nullptr)
        {
            return 0;
        }
        uintptr_t const a = reinterpret_cast<uintptr_t>(p);
        uintptr_t const b = reinterpret_cast<uintptr_t>(q);
        if (b <= a)
        {
            return 0;
        }
        g_granule = static_cast<size_t>(b - a);
        return g_granule;
    }

    // Nothing waits on this probe, so any subset of a probe batch still drains.
    void pool_probe_worker(void*)
    {
        kos_sem_post(CH_DONE);
    }

    // Can this board host `n` workers CONCURRENTLY, right now? A test whose workers wait
    // on EACH OTHER cannot spawn first and drain a partial batch: it must ask before it
    // spawns anything. KICKOS_MAX_THREADS does not answer this; slots already held by
    // service-list drivers and arena room for each stack both count.
    //
    // Call immediately before the real spawns: spawning does not reschedule, so all `n`
    // probes are resident at once, and root is the lowest-priority thread, so each probe
    // reaches exit before root runs again. When wait_n returns every probe slot is EXITED
    // and every probe stack is back on the free list.
    bool pool_can_host(int n)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        int got = 0;
        for (int i = 0; i < n; i++)
        {
            if (not kos::thread::spawn_caps(pool_probe_worker, nullptr, "probe", 10, caps, 1).valid())
            {
                break;
            }
            got++;
        }
        wait_n(got);
        return got == n;
    }

    // --- SVC argument/return roundtrip -----------------------------------------
    // Proves the count comes from the len WE passed, not from a kernel-side walk of the
    // buffer.
    //
    // NOT a delivery check: kos_kconsole_write returns `len` even when console_emit
    // discards every byte (kernel/init/console.cc, USER_OWNED), and userspace has no
    // readback. Delivery is asserted where the transport acknowledges: cap_index0's
    // post-publish arm and the harness's route probe.
    void t_svc()
    {
        char const* s = "# [svc] kconsole_write arg/return roundtrip (not a delivery check)\n";
        size_t const n = strlen(s);
        TAP_CHECK(kos_kconsole_write(s, n) == static_cast<int32_t>(n));
        TAP_CHECK(kos_kconsole_write(s, 0) == 0); // a len-0 write is a legitimate 0 (sys.h)
        // len is honoured: pass a PREFIX (itself a whole line, so the TAP stream stays
        // well formed) and require the short count back; a kernel that strlen'd the
        // buffer would return more and spill the tail marker.
        char const* pfx = "# [svc] len-honoured prefix\nTRAILING-MUST-NOT-APPEAR";
        int32_t const cut = static_cast<int32_t>(strlen("# [svc] len-honoured prefix\n"));
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
        auto a = kos::thread::spawn_caps(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10,
                                         caps, 2);
        auto b = kos::thread::spawn_caps(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10,
                                         caps, 2);
        TAP_CHECK(a.valid() and b.valid()); // spawn failure (e.g. exhausted thread pool) would hang the join
        wait_n(2);
        TAP_CHECK(log_eq("AB")); // A (spawned first, equal prio) runs to completion first
    }

    // --- Priority preempt on ready (thread-ctx sem post) -----------------------
    kos_cap_t g_go = KOS_CAP_NONE;
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
        kos_sem_create(0, &g_go);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_go, CH_FULL}};
        auto hi = kos::thread::spawn_caps(preempt_high, nullptr, "high", 20, caps, 3);
        auto lo = kos::thread::spawn_caps(preempt_low, nullptr, "low", 8, caps, 3);
        TAP_CHECK(hi.valid() and lo.valid()); // spawn failure would hang the join below
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

    // Branch-clock oracle: kos_periph_clock_hz.
    void t_periph_clock_hz()
    {
        // A base no backend models returns 0 on EVERY target, proving the dispatch
        // path + the fallback/backend plumbing reach the arch seam. On the host
        // sim any base returns 0 (no silicon clock), mirroring cpu_clock_hz's sim-0.
        uint32_t const bogus = kos_periph_clock_hz(0xDEAD0000u);
        TAP_CHECK(bogus == 0u);
        TAP_CHECK(bogus == kos_periph_clock_hz(0xDEAD0000u)); // read-only + stable
    }

    // Pin-mux syscall: kos_pinmux_set. An out-of-range port/pin is REJECTED
    // (rc < 0) on every target: -KOS_EINVAL where a chip owns its PORT/IOCR block,
    // -KOS_ENOSYS on the declining-fallback targets (host sim). So garbage is never silently
    // accepted and the dispatch -> arch-seam plumbing is proven. Touches no hardware:
    // both rejects return BEFORE any write.
    //
    // Excluding -KOS_EPERM is what keeps this honest: the AUTH_PINMUX gate runs BEFORE
    // the range check, so a bare `rc < 0` would pass just as happily on a root that had
    // silently lost the bit, and this test would then witness nothing.
    void t_pinmux_set()
    {
        int32_t const bad_port = kos_pinmux_set(99u, 0u, 0x10u);
        int32_t const bad_pin = kos_pinmux_set(0u, 99u, 0x10u);
        TAP_CHECK(bad_port < 0 and bad_port != -KOS_EPERM); // port out of range
        TAP_CHECK(bad_pin < 0 and bad_pin != -KOS_EPERM);   // pin out of range
    }

    // Clock-select seam: kos_cpu_clock_set is PRIVILEGED (syscall gate returns
    // the sentinel 0 == "cannot change" to any unprivileged caller, with NO retune).
    // This test exercises exactly that unprivileged-reject contract. It MUST run from
    // a spawned UNPRIVILEGED child, never from root: a posture in which root is
    // privileged would actually retune on a chip with a real backend (XMC/K64F) and
    // leave the core clock moved for the rest of the suite. The privileged
    // real-retune + coherence tail (re-anchor / baud / re-arm) is covered by the
    // clockretune harness, silicon-only; see docs/design-m3-clock-select.md sec 6.
    uint32_t g_clkset_low = 1; // child: kos_cpu_clock_set(LOW), expect 0 (rejected)
    uint32_t g_clkset_mid = 1;
    uint32_t g_clkset_max = 1;
    kos_cap_t g_clkset_done = KOS_CAP_NONE;
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
        kos_sem_create(0, &g_clkset_done);
        kos_cap_grant caps[] = {{g_clkset_done, CH_FULL}}; // g_clkset_done@1 (CH_DONE)
        auto w = kos::thread::spawn_caps(clkset_unpriv_worker, nullptr, "clkset", 10, caps, 1);
        if (not w.valid())
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
    // The IRQ tests below drive kos_irq_inject, a KICKOS_ENABLE_SELFTEST-only syscall.
    // Without the flag inject is a kernel no-op and these arms would deadlock on a
    // handler that never fires, so the definitions must stay gated together with their
    // registrations in main.
    // --- IRQ-context post (tier 2) ---------------------------------------------
    kos_cap_t g_irq = KOS_CAP_NONE;

    // Needs a line with NO HARDWARE SOURCE, not merely an unclaimed one: kos_irq_attach
    // unmasks, so a wired line would deliver a real interrupt into a test binding.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int IRQ_CTX_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 1;
#else
    constexpr int IRQ_CTX_LINE = KICKOS_SELFTEST_IRQ_BASE + 10;
#endif
    void irq_waiter(void*)
    {
        kos_sem_wait(CH_AUX); // g_irq
        log_put('W');
        kos_sem_post(CH_DONE);
    }
    void irq_injector(void*)
    {
        log_put('i');
        kos_irq_inject(IRQ_CTX_LINE); // ISR posts g_irq -> higher-prio waiter preempts
        log_put('r');
        kos_sem_post(CH_DONE);
    }
    void t_irq()
    {
        log_reset();
        kos_sem_create(0, &g_irq);
        // MUST be checked: unchecked, a refused line leaves no ISR bound, irq_waiter parks
        // forever and wait_n(2) below deadlocks root. TAP_CHECK returns before the spawns.
        TAP_CHECK(kos_irq_attach(IRQ_CTX_LINE, g_irq) == 0); // resolves MAIN's cap (CAP_SIGNAL)
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_irq, CH_FULL}};
        kos_cap_grant icaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto w = kos::thread::spawn_caps(irq_waiter, nullptr, "irqW", 15, wcaps, 3);
        auto inj = kos::thread::spawn_caps(irq_injector, nullptr, "irqI", 8, icaps, 2);
        TAP_CHECK(w.valid() and inj.valid()); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_irq); // reclaim (line 5 stays bound to a now-stale handle -> fails safe)
        TAP_CHECK(log_eq("iWr"));
    }

#endif // KICKOS_ENABLE_SELFTEST (IRQ-context post)

    // --- Round-robin interleave ------------------------------------------------
    // Burn per iteration, ~2 quanta. t_rr rescales it to the target's clock granule; a
    // burn shorter than the slice never gets preempted and the interleave never happens.
    uint64_t g_rr_burn_ns = 2000000ull;
    void rr_worker(void* arg) // caps: done@1, lock@2, gate@3
    {
        // Arrival, posted BEFORE the turnstile: the gate proves both workers were spawned,
        // not that both reached it. A worker still starting when the gate opens lets its
        // peer burn a whole slice alone, and the interleave then measures spawn latency
        // instead of the scheduler. Tens of microseconds of extra bring-up flip it.
        kos_sem_post(CH_DONE);
        stage_wait(3);
        char c = arg_char(arg);
        for (int i = 0; i < 3; i++)
        {
            log_put(c);
            uint64_t start = kos_clock_now();
            while (kos_clock_now() - start < g_rr_burn_ns)
            {
            }
        }
        kos_sem_post(CH_DONE);
    }
    void t_rr()
    {
        // The quantum must be resolvable by the monotonic clock or the slice cannot
        // preempt mid-burn. Never hardcode a fine clock: the QEMU semihosting clock is
        // coarse, and a quantum below its resolution is neither testable nor shippable.
        // Measure a full granule from two consecutive edges (phase-independent); the
        // probe must SPIN, since a WFI would not advance the clock.
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
        kos_sem_create(0, &g_gate);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                {g_gate, CH_FULL}}; // -> done@1, lock@2, gate@3
        // Must stay UNPRIVILEGED: that is what exercises the region reload per slice.
        auto a = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                                         caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                         /*privileged=*/false);
        auto b = kos::thread::spawn_caps(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                                         caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                         /*privileged=*/false);
        TAP_CHECK(g_gate != KOS_CAP_NONE and a.valid() and b.valid()); // spawn failure would hang the join below
        wait_n(2);        // both parked on the turnstile
        stage_release();
        wait_n(2);        // both finished their burns
        kos_handle_close(g_gate);
        // Sustained interleave: each of B's earlier iterations precedes A's next. A
        // pure-FIFO scheduler would run A's three to completion first, so a pass here is
        // not vacuous. Keep the diag: the bare predicate cannot distinguish "A ran to
        // completion" from "they alternated but B started late".
        g_log[g_logn] = 0;
        tap::diag("rr order: %s", g_log);
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
        auto l = kos::thread::spawn_caps(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL",
                                         10, caps, 2);
        auto s = kos::thread::spawn_caps(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS",
                                         10, caps, 2);
        TAP_CHECK(l.valid() and s.valid()); // spawn failure would hang the join below
        wait_n(2);
        TAP_CHECK(log_eq("SL")); // the short sleeper wakes first
    }

    // --- Two equal-priority threads blocking on one semaphore ------------------
    // Regression: the blocker must detach from the ready list before parking on
    // the wait queue (shared link node); without it the second waiter is orphaned
    // and never wakes.
    kos_cap_t g_multi = KOS_CAP_NONE;
    void multi_worker(void* arg)
    {
        kos_sem_wait(CH_AUX); // g_multi
        log_put(arg_char(arg));
        kos_sem_post(CH_DONE);
    }
    void t_multi()
    {
        log_reset();
        kos_sem_create(0, &g_multi);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_multi, CH_FULL}};
        auto a = kos::thread::spawn_caps(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10,
                                         caps, 3);
        auto b = kos::thread::spawn_caps(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10,
                                         caps, 3);
        // A dropped spawn leaves no worker, so main posts to nobody and hangs in wait_n.
        // Fail loud here instead of timing the gate out.
        TAP_CHECK(a.valid() and b.valid());
        kos_sleep_ns(5000000ull); // let both block on g_multi
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        wait_n(2);
        kos_sem_destroy(g_multi); // reclaim
        TAP_CHECK(count('A') == 1 and count('B') == 1); // both woke
    }

#if defined(KICKOS_ENABLE_SELFTEST) // inject-driven (see the tier-2 block above)
    // --- Tier-1 IRQ-as-event: unprivileged userspace driver --------------------
    kos_cap_t g_irqdrv_done = KOS_CAP_NONE;
    // ONE ready-handshake handle shared by every tier-1 IRQ arm below; safe only because
    // they are strictly sequential and each creates and destroys it. Keep it shared: on
    // microbit the arena starts where .bss ends, so a handful of extra file-scope words
    // flips a later arena probe from RUN to SKIP.
    kos_cap_t g_irq_ready = KOS_CAP_NONE;
    void* g_mmio = nullptr; // fake device MMIO word, granted to the driver
    int g_seen[3] = {0, 0, 0};
    constexpr int IRQ_LINE = KICKOS_SELFTEST_IRQ_BASE + 1;

    void irq_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready: holds the line cap + about to park
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
        // Root PLAYS THE DEVICE, so it needs write access to a page it allocated. App
        // static data will not do: it sits outside the arena and cannot be granted to the
        // driver.
        //
        // Alloc BEFORE the sems, or the alloc-fail early return leaks them.
        g_mmio = kos_ram_alloc(4096);
        if (g_mmio == nullptr)
        {
            // A 16 KiB SRAM part (microbit) cannot spare a 4 KiB page for the mock MMIO
            // region. A counted TAP skip, not a pass.
            tap::skip("4 KiB MMIO-page alloc failed -- board too small");
            return;
        }
        // Without this grant the writes below fault: root does not reach its own arena
        // allocations.
        TAP_CHECK(kos_mem_self_grant(g_mmio, 4096) == 0);
        *static_cast<volatile int*>(g_mmio) = 0;
        kos_sem_create(0, &g_irqdrv_done);
        kos_sem_create(0, &g_irq_ready);
        // ROOT must mint the line (the suite declares KOS_AUTH_IRQ); a worker runs at
        // authority 0 and cannot claim for itself, so it gets a WAIT-only copy.
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(IRQ_LINE, KOS_IRQ_EDGE, &irq) == 0);
        // A claim leaves the line MASKED and the ready handshake fires BEFORE the driver's
        // first wait, so arm the line here: otherwise an inject can land on a masked line
        // and the driver's first arm discards it.
        kos_irq_ack(irq);
        kos_cap_grant caps[] = {{g_irqdrv_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(irq_driver, nullptr, "irqdrv", 15, caps, 3,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false, g_mmio, 4096);
        if (not drv.valid())
        {
            kos_sem_destroy(g_irqdrv_done); // reclaim before the failure return
            kos_sem_destroy(g_irq_ready);
        }
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq); // the driver is the sole holder: its exit frees the line
        kos_sem_wait(g_irq_ready);
        for (int i = 1; i <= 3; i++)
        {
            *static_cast<volatile int*>(g_mmio) = 0x100 + i; // "device" produces data
            kos_irq_inject(IRQ_LINE);
            kos_sem_wait(g_irqdrv_done); // serviced + acked
        }
        kos_sem_destroy(g_irqdrv_done); // reclaim (driver has exited: higher prio ran to completion)
        kos_sem_destroy(g_irq_ready);
        TAP_CHECK(g_seen[0] == 0x101 and g_seen[1] == 0x102 and g_seen[2] == 0x103);
    }

    // --- IRQ mask latches-and-coalesces a masked raise -------------------------
    // Driver MUST run below root so root can fire three raises back-to-back. Fire #1
    // delivers and masks the line; #2 and #3 land masked and COALESCE one-deep, so the
    // driver services EXACTLY twice, never a phantom third.
    int g_mask_serviced = 0;
    constexpr int MASK_LINE = KICKOS_SELFTEST_IRQ_BASE + 0;

    void mask_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
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
        kos_sem_create(0, &g_irq_ready);
        g_mask_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(MASK_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_irq_ack(irq); // arm: root injects below and this driver runs BELOW root
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(mask_driver, nullptr, "maskdrv", 1, caps, 3); // below root
        TAP_CHECK(drv.valid());        // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready); // driver holds the line cap, about to wait
        kos_sem_destroy(g_irq_ready);
        // Three back-to-back onto the parked (lower-prio) driver's line: #1 delivers
        // + masks; #2 latches on the masked line; #3 coalesces into that one latch.
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        // Deterministic: block until the two services (the delivery + the single
        // coalesced redelivery) have both landed, with no sleep-based ordering.
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

    // --- An EDGE driver can retire a latch it knows is stale -------------------
    // The inverse of irq_mask_coalesce above, against the same three back-to-back raises.
    // The controller is a reserved block no grant reaches, so kos_irq_discard is a
    // driver's ONLY way to drop a pending it knows is stale. Dropping it gives ONE
    // service where coalescing gives two, which is what makes this arm non-vacuous.
    int g_disc_serviced = 0;
    // This is the one arm that RETIRES a pending, so its line must have no source that can
    // re-assert underneath the ICPR write. A chip that declares such a line wins here,
    // and a peripheral line only passes while nothing drives it. RP2040 IRQ15 is
    // SIO_IRQ_PROC0, which re-asserts from the core-local FIFO level with no enable bit,
    // and the retired latch redelivers.
    // Not the ownership arm's line: t_irq_ownership deliberately leaves that one bound to a
    // stale handle, so sharing it would make this arm depend on registration order to still
    // be claimable.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int DISCARD_LINE = KICKOS_IRQ_SOFT_ONLY_BASE;
#else
    constexpr int DISCARD_LINE = KICKOS_SELFTEST_IRQ_BASE + 9;
#endif

    // A base that moved over either would hand two arms one line, surfacing as an
    // unrelated arm failing.
    static_assert(DISCARD_LINE < KICKOS_SELFTEST_IRQ_BASE
                      or DISCARD_LINE > KICKOS_SELFTEST_IRQ_BASE + 8,
                  "the discard line falls inside the selftest's nine-line IRQ block");
    static_assert(IRQ_CTX_LINE < KICKOS_SELFTEST_IRQ_BASE
                      or IRQ_CTX_LINE > KICKOS_SELFTEST_IRQ_BASE + 8,
                  "the irq-context line falls inside the selftest's nine-line IRQ block");
    static_assert(IRQ_CTX_LINE != DISCARD_LINE,
                  "the irq-context and discard arms would share a line");

    void discard_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
        for (int i = 0; i < 2; i++)
        {
            irq.wait();
            g_disc_serviced++;
            irq.discard(); // the line is masked here: retire anything coalesced onto it
            irq.ack();
            kos_sem_post(CH_DONE);
        }
    }
    void t_irq_discard()
    {
        // A bad cap is refused at the same chokepoint as wait/ack, before any controller
        // write. Must stay ahead of every allocation, or its failure return strands them.
        TAP_CHECK(kos_irq_discard(KOS_CAP_NONE) == -KOS_EBADF);
        kos_sem_create(0, &g_irq_ready);
        g_disc_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(DISCARD_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_irq_ack(irq); // arm: root injects below and this driver runs BELOW root
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(discard_driver, nullptr, "discirq", 1, caps, 3);
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);
        // #1 delivers + masks; #2 and #3 coalesce into one latch on the masked line.
        kos_irq_inject(DISCARD_LINE);
        kos_irq_inject(DISCARD_LINE);
        kos_irq_inject(DISCARD_LINE);
        wait_n(1);
        // Bounded settle: without the discard the coalesced latch redelivers here and
        // the driver reaches its second service (which is what irq_mask_coalesce asserts).
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_disc_serviced == 1); // the latch was retired, not redelivered
        // Liveness: discard must retire the latch without wedging the line, so a fresh
        // raise still delivers and the driver reaches its second service and exits.
        kos_irq_inject(DISCARD_LINE);
        wait_n(1);
        TAP_CHECK(g_disc_serviced == 2);
    }

    // --- Auto-rearm: wait; service with NO explicit ack ------------------------
    // irq_wait re-arms the previously-consumed line itself, so a driver that never acks
    // still receives every subsequent IRQ. Driver MUST run above root, so it reaches its
    // next wait before root injects again.
    int g_autorearm_seen = 0;
    constexpr int AUTO_REARM_LINE = KICKOS_SELFTEST_IRQ_BASE + 2;

    void autorearm_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
        for (int i = 0; i < 3; i++)
        {
            irq.wait(); // no ack: the next wait re-arms the line on its own
            g_autorearm_seen++;
            kos_sem_post(CH_DONE);
        }
    }
    void t_irq_autorearm()
    {
        kos_sem_create(0, &g_irq_ready);
        g_autorearm_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(AUTO_REARM_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line before injecting
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(autorearm_driver, nullptr, "autoirq", 15, caps, 3);
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);
        for (int i = 0; i < 3; i++)
        {
            kos_irq_inject(AUTO_REARM_LINE);
            wait_n(1);
        }
        TAP_CHECK(g_autorearm_seen == 3); // all three delivered without a single ack
    }

    // --- Pitfall-1 regression: no phantom wake in the ack;compute;wait shape ----
    // After an explicit ack re-arms the line, exactly ONE injected event must yield
    // exactly ONE wait-return: the second wait BLOCKS. Setting needs_rearm in the ISR
    // instead of on wait-return unmasks early and phantom-posts, leaving the driver to
    // service an event that never came. Driver MUST run below root so root sequences each
    // step, and every inject below must target an ARMED line.
    int g_phantom_seen = 0;
    constexpr int PHANTOM_LINE = KICKOS_SELFTEST_IRQ_BASE + 4;

    void phantom_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
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
        kos_sem_create(0, &g_irq_ready);
        g_phantom_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(PHANTOM_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_irq_ack(irq); // arm: every inject below must target an ARMED line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(phantom_driver, nullptr, "phantirq", 1, caps, 3); // below root
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);

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
        kos_cap_t h = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &h) == 0);
        TAP_CHECK(kos_sem_destroy(h) == 0);          // live handle destroys
        TAP_CHECK(kos_sem_destroy(h) == -KOS_EBADF); // stale handle rejected (gen bumped)
        kos_cap_t h2 = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &h2) == 0 and h2 != h); // reused slot carries a fresh generation
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        // Malformed caps must fail with the SPECIFIC code -KOS_EBADF, not any negative.
        // handle_close is the probe because it is the one cap syscall that returns a value;
        // wait/post share the same cap_resolve chokepoint.
        TAP_CHECK(kos_handle_close(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -KOS_EBADF);
        // The count is bounded at both ends: birth outside [0, KOS_SEM_COUNT_MAX] is
        // refused, and a post at the ceiling with no waiter is refused rather than
        // overflowing. Creating at the ceiling is what makes the refusal reachable.
        kos_cap_t bad = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(-1, &bad) == -KOS_EINVAL and bad == KOS_CAP_NONE);
        kos_cap_t hmax = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(KOS_SEM_COUNT_MAX, &hmax) == 0
                  and kos_sem_post(hmax) == -KOS_EOVERFLOW
                  and kos_handle_close(hmax) == 0);
    }

    // --- Refcounted close of a DELEGATED sem: object survives while a co-holder is
    // parked; the last close frees it. Under per-thread caps, closing MY cap never
    // destroys an object another thread still holds.
    kos_cap_t g_dsem = KOS_CAP_NONE;
    void destroy_waiter(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        kos_sem_wait(CH_READY); // g_dsem: parks (initial 0)
        kos_sem_post(CH_DONE);
    }
    void destroy_poster(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        // Sleep past MAIN's close below, THEN post: the wake of the parked waiter
        // happens strictly AFTER MAIN has dropped its own (shared) cap on g_dsem.
        kos_sleep_ns(10000000ull);
        kos_sem_post(CH_READY); // wakes destroy_waiter
        kos_sem_post(CH_DONE);
    }
    void t_sem_destroy_busy()
    {
        kos_sem_create(0, &g_dsem);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_dsem, CH_FULL}}; // done@1, dsem@2
        auto w = kos::thread::spawn_caps(destroy_waiter, nullptr, "dwaiter", 15, caps, 2);
        auto p = kos::thread::spawn_caps(destroy_poster, nullptr, "dposter", 15, caps, 2);
        TAP_CHECK(w.valid() and p.valid()); // spawn failure would hang wait_n(2) below
        kos_sleep_ns(2000000ull);     // let the waiter park on g_dsem; refs = main+waiter+poster = 3
        // Close MAIN's cap WHILE the waiter is parked and the poster has not yet posted:
        // refs 3->2, so the object MUST survive (co-holders still name it). If this freed
        // or corrupted the object or its wait queue, the poster's later post would not
        // wake the parked waiter and wait_n(2) would hang.
        TAP_CHECK(kos_handle_close(g_dsem) == 0);
        wait_n(2); // both reported => object + wait queue intact after MAIN's close
        // Both holders have exited, so refs -> 0. Create/close well past the pool size
        // must never exhaust, which is only true if that last close reclaimed the slot.
        for (int i = 0; i < 100; i++)
        {
            kos_cap_t s = KOS_CAP_NONE;
            TAP_CHECK(kos_sem_create(0, &s) == 0 and kos_handle_close(s) == 0);
        }
    }

    // --- Owning kos::Semaphore RAII --------------------------------------------
    void t_sem_raii()
    {
        // Scoped create/destroy, well past the ~16-slot pool, must not exhaust it.
        for (int i = 0; i < 100; i++)
        {
            kos::Semaphore s;
            TAP_CHECK(s.valid());
        }
        // Move-construct empties the source, so scope exit destroys once.
        kos::Semaphore a;
        kos_cap_t aid = a.id();
        kos::Semaphore b(static_cast<kos::Semaphore&&>(a));
        TAP_CHECK(b.id() == aid and not a.valid());

        // Move-assign onto a live handle: the old target is destroyed, source emptied.
        kos::Semaphore c;
        c = static_cast<kos::Semaphore&&>(b);
        TAP_CHECK(c.id() == aid and not b.valid());

        // Self-move-assign is a no-op (must not destroy its own handle). Aliased
        // through a reference so the compiler's -Wself-move doesn't fire.
        kos::Semaphore& cref = c;
        c = static_cast<kos::Semaphore&&>(cref);
        TAP_CHECK(c.id() == aid);
    }

    // --- PI mutex: basic lock/unlock + mutual exclusion (H1) -------------------
    // Three equal-priority workers each do ITERS non-atomic read-yield-write cycles under
    // the mutex. The kos_yield() inside the critical section is what makes the arm
    // non-vacuous: without serialization the peer reads the stale value and updates are
    // lost (final < expected), so exact conservation is the only passing outcome.
    constexpr int MTX_ITERS = 20;
    int g_mtx_shared = 0;
    // A mutex cap carries CAP_TRANSFER only (possession IS the lock/unlock authority,
    // no WAIT/SIGNAL split), so it must be delegated with a TRANSFER-only mask: a
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
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        g_mtx_shared = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2
        auto a = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbA", 10, caps, 2);
        auto b = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbB", 10, caps, 2);
        auto c = kos::thread::spawn_caps(mtx_basic_worker, nullptr, "mbC", 10, caps, 2);
        if (not a.valid() or not b.valid() or not c.valid())
        {
            // A 2-slot pool (microbit) cannot host 3 workers. The partial batch MUST be
            // drained: they post the shared g_done, and a stale post desyncs a later
            // wait_n. Close the mutex too, or the leak cascades through the cap table.
            int n = 0;
            if (a.valid()) { n++; }
            if (b.valid()) { n++; }
            if (c.valid()) { n++; }
            wait_n(n);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(g_mtx_shared == 3 * MTX_ITERS); // no lost update -> mutual exclusion held
    }

    // The PI choreography below holds only if the lock/block/boost chain forms within the
    // slack between scheduled wakes, so the unit must dominate a reschedule round-trip,
    // NOT merely the clock granule. On armv6m (software 64-bit divides in the tickless
    // math) that round-trip is ~10-30 ms, far above a 1 ms unit. The 1 ms floor keeps fast
    // boards where they were; the cap stops a pathological reading stretching the run.
    uint64_t mtx_time_unit()
    {
        // A unit below a few granules is unmeasurable, so the granule is a lower bound.
        uint64_t g0 = kos_clock_now();
        uint64_t g1 = g0;
        while (g1 == g0) { g1 = kos_clock_now(); }
        uint64_t g2 = g1;
        while (g2 == g1) { g2 = kos_clock_now(); }
        uint64_t granule = g2 - g1;

        // Per-sleep OVERHEAD above a small real sleep: the jitter a 1-unit gap must
        // out-scale.
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
    // low(8) holds the mutex and busy-spins; high(20) wakes mid-spin and blocks on it,
    // boosting low to 20; med(12) wakes next and must NOT preempt the boosted low. The
    // observables: 'u' before 'm' (boost held), and 'm' before 'z' (low reverted to base 8
    // on unlock). Both orderings invert if the boost or the revert is missing.
    uint64_t g_mtx_unit = 1000000ull;
    void pi_low(void*) // caps: done@1, lock@2, mutex@3, gate@4
    {
        stage_wait(4);
        kos_mutex_lock(3);
        log_put('l');
        mtx_spin(g_mtx_unit * 4); // hold across high's and med's wake instants
        log_put('u');
        kos_mutex_unlock(3); // hands off to high (preempts here); low reverts to base
        log_put('z');        // reached only after med (12) has run -> proves revert
        kos_sem_post(CH_DONE);
    }
    void pi_high(void*) // caps: done@1, lock@2, mutex@3, gate@4
    {
        stage_wait(4);
        kos_sleep_ns(g_mtx_unit * 1);
        log_put('h');
        kos_mutex_lock(3); // low holds it -> block + boost low to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void pi_med(void*) // caps: done@1, lock@2, gate@3
    {
        stage_wait(3);
        kos_sleep_ns(g_mtx_unit * 2);
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_pi()
    {
        log_reset();
        g_mtx_unit = mtx_time_unit();
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX},
                                 {g_gate, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL}};
        auto lo = kos::thread::spawn_caps(pi_low, nullptr, "piLo", 8, lcaps, 4);
        auto hi = kos::thread::spawn_caps(pi_high, nullptr, "piHi", 20, lcaps, 4);
        auto md = kos::thread::spawn_caps(pi_med, nullptr, "piMd", 12, mcaps, 3);
        stage_release();
        if (not lo.valid() or not hi.valid() or not md.valid())
        {
            // A 2-slot pool cannot host 3 workers. The partial batch MUST be drained (they
            // post the shared g_done) and the mutex closed, or a later wait_n desyncs.
            int n = 0;
            if (lo.valid()) { n++; }
            if (hi.valid()) { n++; }
            if (md.valid()) { n++; }
            wait_n(n);
            kos_handle_close(m);
            kos_handle_close(g_gate);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        kos_handle_close(g_gate);
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
    // PROPAGATE two hops, raising C to A's priority. D is ready while C spins, so 'e'
    // before 'd' can only hold if the boost travelled B -> C.
    //
    // Handed along C -> B -> C -> A -> D by semaphore, so the arm holds no sleep deadline.
    // Two semaphores, not one: a post is popped by the HIGHEST-priority waiter, so a token
    // B needs cannot be posted anywhere A and D are also waiting.
    //
    // A is released by C, not by B: a release from B would reach A while B still merely HOLDS
    // M1, and the arm would then witness two single-hop walks a one-hop kernel reproduces.
    kos_cap_t g_ch_up = KOS_CAP_NONE; // C <-> B: M2 is held, then M1 is held and B is about to block
    kos_cap_t g_ch_on = KOS_CAP_NONE; // C -> A -> D: B is blocked on M2, then the chain has formed
    void ch_c(void*) // caps: done@1, lock@2, M2@3, gate@4, up@5, on@6
    {
        stage_wait(4);
        kos_mutex_lock(3); // M2
        log_put('c');
        kos_sem_post(5);   // M2 is held: B may take M1
        // B hands 5 back before blocking on M2, and B's block is what boosts us onto the CPU,
        // so getting past this wait means B is ALREADY a waiter on M2.
        kos_sem_wait(5);
        kos_sem_post(6);          // only now may A block on M1
        mtx_spin(g_mtx_unit * 8); // A blocks on M1 inside this
        log_put('e');
        kos_mutex_unlock(3);
        log_put('C');
        kos_sem_post(CH_DONE);
    }
    void ch_b(void*) // caps: done@1, lock@2, M1@3, M2@4, up@5
    {
        kos_sem_wait(5);
        kos_mutex_lock(3); // M1 (before A tries it)
        log_put('b');
        kos_sem_post(5);   // C is the only waiter and it is below us, so this does not preempt
        kos_mutex_lock(4); // M2: C holds it -> block, boost C to 10, which resumes C's wait
        kos_mutex_unlock(4);
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_a(void*) // caps: done@1, lock@2, M1@3, on@4
    {
        kos_sem_wait(4);
        kos_sem_post(4);   // releases D, which we outrank, so the chain forms before it runs
        kos_mutex_lock(3); // M1: B holds it AND waits on M2 -> boost B to 20, then C to 20
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_d(void*) // caps: done@1, lock@2, on@3
    {
        // Index 3, not the 4 A posts on: holding no mutex cap shifts the same semaphore one
        // slot down. Waiting on the wrong index returns at once and 'd' precedes 'e'.
        kos_sem_wait(3);
        log_put('d');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_chain()
    {
        // Ask the pool BEFORE creating anything: the three staging semaphores exceed the
        // supply on the small boards, so this stays a skip there instead of a create failure.
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (4 interdependent workers)");
            return;
        }
        log_reset();
        g_mtx_unit = mtx_time_unit();
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        int const rc1 = kos_mutex_create(&m1);
        int const rc2 = kos_mutex_create(&m2);
        TAP_CHECK(rc1 == 0 and rc2 == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_up) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_on) == 0);
        // Six grants, exactly KICKOS_MAX_SPAWN_GRANTS.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m2, CH_MTX},
                                 {g_gate, CH_FULL}, {g_ch_up, CH_FULL}, {g_ch_on, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}, {g_ch_up, CH_FULL}};
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX},
                                 {g_ch_on, CH_FULL}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ch_on, CH_FULL}};
        auto c = kos::thread::spawn_caps(ch_c, nullptr, "chC", 5, ccaps, 6);
        auto b = kos::thread::spawn_caps(ch_b, nullptr, "chB", 10, bcaps, 5);
        auto a = kos::thread::spawn_caps(ch_a, nullptr, "chA", 20, acaps, 4);
        auto d = kos::thread::spawn_caps(ch_d, nullptr, "chD", 15, dcaps, 3);
        stage_release();
        // The probe above just held four slots and four stacks, so a failure now is a pool
        // bug, not a small board.
        TAP_CHECK(c.valid() and b.valid() and a.valid() and d.valid());
        wait_n(4);
        kos_handle_close(g_gate);
        kos_handle_close(g_ch_up);
        kos_handle_close(g_ch_on);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        TAP_CHECK(count('c') == 1 and count('e') == 1 and count('d') == 1
                  and count('b') == 1 and count('C') == 1);
        TAP_CHECK(nth('b', 1) < nth('e', 1)); // chain formed (B took M1 before C released M2)
        TAP_CHECK(nth('e', 1) < nth('d', 1)); // CHAIN BOOST: C ran above med across two hops
    }

    // --- Owner dies holding the mutex: waiter gets OWNER_DIED (H7, R3) ----------
    // Sleep-sequenced so it does not depend on main's posts preempting synchronously. The
    // owner must exit WHILE still holding: that is what makes cap_teardown force-unlock
    // and the woken waiter's lock() return OWNER_DIED.
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
        // mutex would be stranded. A plain `>= 0` test would wrongly skip this, since
        // owner-died is a NEGATIVE code: special-case it as held.
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
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t holds = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const hrc = kos_sem_create(0, &holds);
        TAP_CHECK(mrc == 0 and hrc == 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}}; // mtx@1, holds@2
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mtx@2
        auto ow = kos::thread::spawn_caps(od_owner, nullptr, "odOwn", 8, ocaps, 2);
        auto wt = kos::thread::spawn_caps(od_waiter, nullptr, "odWt", 12, wcaps, 2);
        TAP_CHECK(ow.valid() and wt.valid());
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
    // Keep the FIRST refusal of a batch: a later create cannot see a fuller supply than the
    // one before it, so the first is what binds this board.
    void note_refusal(int rc, int* first)
    {
        if (rc != 0 and *first == 0)
        {
            *first = rc;
        }
    }
    void t_mutex_deadlock()
    {
        // Self-deadlock: a recursive lock is refused (-KOS_EDEADLK), not parked, and leaves
        // the mutex holdable/releasable normally.
        kos_cap_t self = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == -KOS_EDEADLK); // recursive -> refused
        TAP_CHECK(kos_mutex_unlock(self) == 0);
        TAP_CHECK(kos_handle_close(self) == 0);

        // Cross-thread cycle: A owns M1 + waits M2; B owns M2 + tries M1 -> -KOS_EDEADLK.
        g_cyc_rb = -99;
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        kos_cap_t have1 = KOS_CAP_NONE;
        kos_cap_t have2 = KOS_CAP_NONE;
        kos_cap_t goA = KOS_CAP_NONE;
        kos_cap_t goB = KOS_CAP_NONE;
        int refused = 0;
        note_refusal(kos_mutex_create(&m1), &refused);
        note_refusal(kos_mutex_create(&m2), &refused);
        note_refusal(kos_sem_create(0, &have1), &refused);
        note_refusal(kos_sem_create(0, &have2), &refused);
        note_refusal(kos_sem_create(0, &goA), &refused);
        note_refusal(kos_sem_create(0, &goB), &refused);
        if (m1 == KOS_CAP_NONE or m2 == KOS_CAP_NONE or have1 == KOS_CAP_NONE
            or have2 == KOS_CAP_NONE or goA == KOS_CAP_NONE or goB == KOS_CAP_NONE)
        {
            // The cycle needs 2 mutexes and 4 sems live at once, which the small boards
            // cannot hold. No worker has spawned yet, so reclaiming in any order is safe.
            if (m1 != KOS_CAP_NONE) { kos_handle_close(m1); }
            if (m2 != KOS_CAP_NONE) { kos_handle_close(m2); }
            if (have1 != KOS_CAP_NONE) { kos_sem_destroy(have1); }
            if (have2 != KOS_CAP_NONE) { kos_sem_destroy(have2); }
            if (goA != KOS_CAP_NONE) { kos_sem_destroy(goA); }
            if (goB != KOS_CAP_NONE) { kos_sem_destroy(goB); }
            // WHICH supply ran out is the diagnosis: -KOS_EMFILE is this thread's capability
            // table (a declared-demand fix), anything else is an object pool.
            char const* why = "pool too small";
            if (refused == -KOS_EMFILE)
            {
                why = "cap table too small (6 concurrent caps)";
            }
            tap::skip("%s", why);
            return;
        }
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {m1, CH_MTX}, {m2, CH_MTX},
                                 {have1, CH_FULL}, {goA, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {m2, CH_MTX}, {m1, CH_MTX},
                                 {have2, CH_FULL}, {goB, CH_FULL}};
        auto a = kos::thread::spawn_caps(cyc_a, nullptr, "cycA", 10, acaps, 5);
        auto b = kos::thread::spawn_caps(cyc_b, nullptr, "cycB", 10, bcaps, 5);
        TAP_CHECK(a.valid() and b.valid());
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
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == -KOS_EBUSY); // refused: you cannot close a mutex you hold
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);  // released -> close now succeeds
    }

    // --- Multiple held mutexes: revert is recompute, NOT restore-to-base (H3) ---
    // B (base 6) holds M1 and M2; H (20) waits on M1, boosting B to 20; D (12) competes.
    // B unlocks M2 while H still waits on M1, so recompute keeps B at 20 and H runs before
    // D. A restore-to-base bug drops B to 6 at the M2 unlock and D runs first, so
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
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        int const rc1 = kos_mutex_create(&m1);
        int const rc2 = kos_mutex_create(&m2);
        TAP_CHECK(rc1 == 0 and rc2 == 0);
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}};
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto b = kos::thread::spawn_caps(mh_b, nullptr, "mhB", 6, bcaps, 4);
        auto h = kos::thread::spawn_caps(mh_h, nullptr, "mhH", 20, hcaps, 3);
        auto d = kos::thread::spawn_caps(mh_d, nullptr, "mhD", 12, dcaps, 2);
        if (not b.valid() or not h.valid() or not d.valid())
        {
            // A 2-slot pool cannot host 3 workers. The partial batch MUST be drained (they
            // post the shared g_done) and both mutexes closed, or a later wait_n desyncs.
            int n = 0;
            if (b.valid()) { n++; }
            if (h.valid()) { n++; }
            if (d.valid()) { n++; }
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
    // The owner check is reachable from an untrusted caller, so it must return an
    // error code here, never panic.
    int g_nonowner_rc = -99;
    void nonowner_unlock(void*) // caps: done@1, mutex@2
    {
        g_nonowner_rc = kos_mutex_unlock(2); // caller is not the owner -> -KOS_EPERM
        kos_sem_post(CH_DONE);
    }
    void t_mutex_unlock_errors()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == -KOS_EPERM); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2
        auto w = kos::thread::spawn_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w.valid());
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
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t holds = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const hrc = kos_sem_create(0, &holds);
        TAP_CHECK(mrc == 0 and hrc == 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}}; // mtx@1, holds@2
        auto ow = kos::thread::spawn_caps(od_solo_owner, nullptr, "odSolo", 8, ocaps, 2);
        TAP_CHECK(ow.valid());
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
        kos_cap_t m = KOS_CAP_NONE; // refs = 1 (main)
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}}; // done@1, mutex@2 (refs -> 2)
        auto w = kos::thread::spawn_caps(deleg_closer, nullptr, "delcl", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        // Child closed its cap (and exited): the object must survive on main's cap.
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0); // last close frees it
        // Pool honesty: create/close well past the pool must not exhaust.
        for (int i = 0; i < 40; i++)
        {
            kos_cap_t x = KOS_CAP_NONE;
            TAP_CHECK(kos_mutex_create(&x) == 0 and kos_handle_close(x) == 0);
        }
    }

    // --- The userspace SPSC byte ring ------------------------------------------
    // Pure logic, no syscalls, so it runs on every board. Buffer and ring MUST stay on the
    // stack: a static here comes out of the tiny boards' user arena and pushes a later
    // arena probe into a skip.
    void t_byte_ring()
    {
        unsigned char buf[8];
        struct kos_byte_ring r;
        kos_byte_ring_init(&r, buf, sizeof(buf));
        // Capacity is size-1: one slot is reserved so head == tail is unambiguously empty.
        TAP_CHECK(kos_byte_ring_used(&r) == 0);
        TAP_CHECK(kos_byte_ring_space(&r) == 7);

        unsigned char const src[4] = {'a', 'b', 'c', 'd'};
        TAP_CHECK(kos_byte_ring_push(&r, src, 4) == 4);
        TAP_CHECK(kos_byte_ring_used(&r) == 4);
        TAP_CHECK(kos_byte_ring_space(&r) == 3);

        // A short accept, NOT an error and NOT a silent drop: the caller decides.
        TAP_CHECK(kos_byte_ring_push(&r, src, 4) == 3);
        TAP_CHECK(kos_byte_ring_space(&r) == 0);
        TAP_CHECK(kos_byte_ring_push(&r, src, 1) == 0); // full: accepts nothing

        unsigned char out[8] = {0};
        TAP_CHECK(kos_byte_ring_pop(&r, out, 2) == 2);
        TAP_CHECK(out[0] == 'a' and out[1] == 'b');
        // Wrap: the four pushed below straddle the end of the buffer, so a mask bug
        // shows up as wrong ORDER here rather than as a bad count.
        TAP_CHECK(kos_byte_ring_push(&r, src, 2) == 2);
        unsigned char one = 0;
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 1);
        TAP_CHECK(one == 'c');
        TAP_CHECK(kos_byte_ring_pop(&r, out, sizeof(out)) == 6);
        TAP_CHECK(out[0] == 'd' and out[1] == 'a' and out[2] == 'b' and out[3] == 'c');
        TAP_CHECK(out[4] == 'a' and out[5] == 'b');
        TAP_CHECK(kos_byte_ring_used(&r) == 0);
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 0); // empty: nothing to take

        // A non-power-of-two size is a programming error and is REFUSED rather than
        // masked wrong: the ring reports empty-and-full instead of corrupting memory.
        struct kos_byte_ring bad;
        kos_byte_ring_init(&bad, buf, 6);
        TAP_CHECK(kos_byte_ring_space(&bad) == 0);
        TAP_CHECK(kos_byte_ring_push(&bad, src, 1) == 0);
    }

    // --- Per-grant destination indices: the refusals ---------------------------
    // A bad placement list must be REFUSED before anything is built, which is why the
    // checks sit ahead of the slot claim. The positive path is covered by t_irq_reclaim.
    // Both spawns below must fail, so this body is deliberately never reached.
    void capdest_never_runs(void*) { kos_exit(0); }

    // Posts the completion sem from a NON-default index. If placement were ignored the cap
    // would sit at index 1, this post would fail, and root would never be released, so the
    // failure surfaces as a TRUNCATED run rather than a `not ok`. Do not add a report
    // channel: two more file-scope words starve microbit's arena.
    void capdest_probe(void*) { kos_sem_post(CH_IRQ); }

    void t_cap_dest()
    {
        kos_cap_t sem = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &sem) == 0);
        kos_cap_grant caps[] = {{sem, CH_FULL}, {sem, CH_FULL}};

        // Two grants naming one slot: the second install would overwrite the first and
        // leak its reference, so the list is refused whole.
        uint16_t const collide[] = {CH_DONE, CH_DONE};
        TAP_CHECK(kos::thread::spawn_caps(capdest_never_runs, nullptr, "cd1", 10, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                          collide)
                      .error() == -KOS_EINVAL);

        // A destination past the child's table. cap.h caps KICKOS_MAX_HANDLES at
        // KCAP_RESERVED_INDEX, so index 65535 is out of range on every board.
        uint16_t const far_off[] = {CH_DONE, 65535};
        TAP_CHECK(kos::thread::spawn_caps(capdest_never_runs, nullptr, "cd2", 10, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                          far_off)
                      .error() == -KOS_EINVAL);

        // A collision with a DEFAULTED entry counts too: entry 0 defaults to index 1 and
        // entry 1 names it explicitly.
        uint16_t const vs_default[] = {0, CH_DONE};
        TAP_CHECK(kos::thread::spawn_caps(capdest_never_runs, nullptr, "cd3", 10, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                          vs_default)
                      .error() == -KOS_EINVAL);

        // The POSITIVE half, and what makes the feature falsifiable: delegate the
        // completion sem at index 3 with nothing at 1 or 2. Ignoring the destination puts
        // it at 1 and the worker's post never lands.
        kos_cap_grant one[] = {{g_done, CH_FULL}};
        uint16_t const at3[] = {CH_IRQ};
        auto const w = kos::thread::spawn_caps(capdest_probe, nullptr, "cdp", 15, one, 1,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                               at3);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(sem);
            return;
        }
        wait_n(1);
        kos_sem_destroy(sem);
    }

    // --- The published console's CRLF expansion --------------------------------
    // Must run on EVERY board, including the ones that do not cook: write_console's cook
    // call site is #if KICKOS_CONSOLE_CRLF, so otherwise the expansion ships with in-env
    // coverage on no board at all. The write-policy legs below are here for the same
    // reason, both being pure functions no transport gate reaches. Keep it pure stack:
    // microbit's arena is 16 KiB and a `static` comes straight out of it.
    void t_console_crlf()
    {
        unsigned char out[16];
        uint32_t taken = 0;

        // Identity when there is nothing to expand.
        unsigned char const plain[3] = {'a', 'b', 'c'};
        TAP_CHECK(kickos::console::cook_crlf(plain, 3, out, sizeof(out), &taken) == 3);
        TAP_CHECK(taken == 3);
        TAP_CHECK(out[0] == 'a' and out[1] == 'b' and out[2] == 'c');

        // Every '\n' gains a '\r' before it.
        unsigned char const nl[3] = {'a', '\n', 'b'};
        TAP_CHECK(kickos::console::cook_crlf(nl, 3, out, sizeof(out), &taken) == 4);
        TAP_CHECK(taken == 3);
        TAP_CHECK(out[0] == 'a' and out[1] == '\r' and out[2] == '\n' and out[3] == 'b');

        // The rule is the kernel's and it does NOT look back: an input that already
        // carries "\r\n" becomes "\r\r\n". A doubled CR is a no-op on the wire, and
        // matching kconsole_write exactly is what keeps the two console routes agreeing.
        unsigned char const crnl[2] = {'\r', '\n'};
        TAP_CHECK(kickos::console::cook_crlf(crnl, 2, out, sizeof(out), &taken) == 3);
        TAP_CHECK(out[0] == '\r' and out[1] == '\r' and out[2] == '\n');

        // A '\n' is never split from its '\r' at the chunk boundary: with room for one
        // more byte the expansion stops BEFORE it, so `taken` is a clean resume point.
        unsigned char const tight[2] = {'x', '\n'};
        TAP_CHECK(kickos::console::cook_crlf(tight, 2, out, 2, &taken) == 1);
        TAP_CHECK(taken == 1);
        TAP_CHECK(out[0] == 'x');
        // Resuming from there emits the pair whole.
        TAP_CHECK(kickos::console::cook_crlf(tight + taken, 1, out, 2, &taken) == 2);
        TAP_CHECK(taken == 1);
        TAP_CHECK(out[0] == '\r' and out[1] == '\n');

        // No output room at all consumes nothing rather than dropping an input byte.
        TAP_CHECK(kickos::console::cook_crlf(plain, 3, out, 0, &taken) == 0);
        TAP_CHECK(taken == 0);

        // The write policy KOS_UART_SET_MODE carries. One pure function decides it for
        // every transport, so both service layers refuse and accept identically.
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{0};
        // A service with no unframed console arm REFUSES: a stored mode nothing reads would
        // tell a caller byte loss was enabled while its writes still blocked.
        TAP_CHECK(kickos::console::mode_apply(nullptr, KOS_UART_F_NONBLOCK, 0u)
                  == -KOS_ENOSYS);
        // An unknown bit is refused whole rather than masked, and stores nothing.
        TAP_CHECK(kickos::console::mode_apply(&mode, 0x80u, 0u) == -KOS_EINVAL);
        TAP_CHECK(mode == 0);
        // Accepted and readable back, which is what the console arm consults per write.
        TAP_CHECK(kickos::console::mode_apply(&mode, KOS_UART_F_NONBLOCK, 0u) == 0);
        TAP_CHECK(mode == KOS_UART_F_NONBLOCK);
        // Clearing it restores the paced default; the flag is not a one-way latch.
        TAP_CHECK(kickos::console::mode_apply(&mode, 0u, 0u) == 0);
        TAP_CHECK(mode == 0);
        // A transport that cannot honour a blocking write REQUIRES the flag and refuses to
        // clear it, so a caller is told it cannot have back-pressure instead of being handed
        // an unbounded wait. The refusal stores nothing.
        mode = KOS_UART_F_NONBLOCK;
        TAP_CHECK(kickos::console::mode_apply(&mode, 0u, KOS_UART_F_NONBLOCK)
                  == -KOS_ENOTSUP);
        TAP_CHECK(mode == KOS_UART_F_NONBLOCK);
        TAP_CHECK(kickos::console::mode_apply(&mode, KOS_UART_F_NONBLOCK,
                                             KOS_UART_F_NONBLOCK) == 0);

        // A non-blocking write REPORTS its short accept, which is the whole difference
        // between this mode and dropping in silence: the service arm turns the shortfall
        // into stats.tx_dropped, the only channel an unframed writer can read.
        unsigned char rbuf[8];
        struct kos_byte_ring ring;
        kos_byte_ring_init(&ring, rbuf, sizeof(rbuf));
        struct kos_uart_stats st = {};
        unsigned char twelve[12];
        memset(twelve, 'z', sizeof(twelve));
        // Eight bytes of room for twelve offered: a short accept, not a refusal and not a
        // wait. The blocking arm cannot be exercised here, having no consumer to terminate.
        uint32_t const nb = kickos::console::write_console(&ring, &st, twelve, 12,
                                                          KOS_UART_F_NONBLOCK);
        TAP_CHECK(nb < 12);       // short accept
        TAP_CHECK(12u - nb > 0u); // the shortfall the service arm charges to tx_dropped
        uint32_t const cooked = st.tx_bytes.load(std::memory_order_relaxed);
        TAP_CHECK(cooked > 0u and cooked <= sizeof(rbuf));
        // The return is INPUT bytes and the counter is COOKED bytes, so these differ under
        // KICKOS_CONSOLE_CRLF: a partially accepted cooked chunk reports no input progress
        // at all. That makes the charge OVER-count by up to one chunk, never under-count,
        // and over-counting is the safe direction: a caller is told it lost at least what it
        // lost. Asserting equality here would pass on the sim, the ONLY crlf=0 tree, and
        // fail on all thirteen boards.
        TAP_CHECK(cooked >= nb);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- One driver per line: a second claim on a bound line is refused --------
    void t_irq_ownership()
    {
        // This arm POISONS the line (below), so it must not be shared with any other arm
        // whatever the registration order.
        constexpr int LINE = KICKOS_SELFTEST_IRQ_BASE + 5;
        kos_cap_t sem = KOS_CAP_NONE;
        kos_sem_create(0, &sem);
        TAP_CHECK(kos_irq_attach(LINE, sem) == 0);          // first claim wins
        TAP_CHECK(kos_irq_attach(LINE, sem) == -KOS_EBUSY); // second is refused (no steal)
        // Tier-1 cannot steal it either. Root holds KOS_AUTH_IRQ, so this is the
        // one-owner-per-line refusal and not the authority gate.
        kos_cap_t stolen = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(LINE, KOS_IRQ_EDGE, &stolen) == -KOS_EBUSY
                  and stolen == KOS_CAP_NONE);
        kos_sem_destroy(sem); // reclaim (line 11 stays bound to a now-stale handle -> fails safe)
    }

    // --- The tier-1 mint is gated on KOS_AUTH_IRQ ------------------------------
    // The refusal MUST be witnessed from a worker, not from root: the suite declares
    // KOS_AUTH_IRQ, so a root-side claim tests the GRANT and can never see the refusal.
    // Keep this to ONE new static: on a 16 KiB part the user arena is what is left after
    // static RAM, so .bss added here shrinks the arena for every later arm.
    int g_claimgate_rc = 0;
    constexpr int CLAIM_GATE_LINE = KICKOS_SELFTEST_IRQ_BASE + 7;

    void claimgate_worker(void*) // UNPRIVILEGED, authority 0; caps: g_done@1 (CH_DONE)
    {
        kos_cap_t line = KOS_CAP_NONE;
        g_claimgate_rc = kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &line);
        kos_sem_post(CH_DONE);
    }
    void t_irq_claim_gate()
    {
        g_claimgate_rc = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // g_done@1 (CH_DONE)
        // ABOVE root, like the other tier-1 workers, so it runs to completion including
        // its exit before root is scheduled again. Its slot is then EXITED and reusable
        // instead of overlapping the next arm's worker.
        auto w = kos::thread::spawn_caps(claimgate_worker, nullptr, "claimgate", 15, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // EPERM, not EBUSY: the line is free, so only the authority gate can refuse it.
        TAP_CHECK(g_claimgate_rc == -KOS_EPERM);
        // And the refusal left NOTHING behind: root can still claim that same line.
        kos_cap_t owned = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &owned) == 0);
        TAP_CHECK(kos_handle_close(owned) == 0);
    }

    // --- A line comes back when its holder dies --------------------------------
    // cap_teardown drops the dying thread's line cap and irq_ref_drop detaches the line
    // and frees the binding slot, so the SAME line is claimable again. Without that
    // release the line keeps the dead driver's handler and returns -KOS_EBUSY forever.
    constexpr int RECLAIM_LINE = KICKOS_SELFTEST_IRQ_BASE + 8;

    // Arms the line via ack rather than parking in wait: the release path must be exercised
    // from the ARMED state, and an ack reaches it without needing an event. Do NOT rewrite
    // this as wait-then-inject: a claim leaves the line masked and spawn does not preempt,
    // so root's inject lands masked and the worker's own first arm discards the latch,
    // giving a deadlock rather than a test.
    void reclaim_worker(void*) // holds the delegated line cap at CH_IRQ, then EXITS
    {
        kos_irq_ack(CH_IRQ);
        kos_sem_post(CH_DONE);
    }
    void t_irq_reclaim()
    {
        kos_cap_t first = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(RECLAIM_LINE, KOS_IRQ_EDGE, &first) == 0);
        // done@1, line@3, index 2 deliberately EMPTY.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {first, KOS_CAP_WAIT}};
        uint16_t const dest[] = {CH_DONE, CH_IRQ};
        auto w = kos::thread::spawn_caps(reclaim_worker, nullptr, "reclaim", 15, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                         /*authority=*/0, dest);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_handle_close(first);
            return;
        }
        // Root drops its copy BEFORE the worker dies: the worker is then the ONLY
        // holder, so its exit is what takes the refcount to zero. Closing after would
        // prove the free path but not that DEATH releases the line.
        TAP_CHECK(kos_handle_close(first) == 0);
        wait_n(1);
        // The worker is higher priority, so it has run to completion (and exited) by
        // the time root is scheduled again. Claiming the same line must now succeed.
        kos_cap_t second = KOS_CAP_NONE;
        // -KOS_EBUSY here means death did not release the line
        TAP_CHECK(kos_irq_claim(RECLAIM_LINE, KOS_IRQ_EDGE, &second) == 0);
        TAP_CHECK(kos_handle_close(second) == 0);
    }

    // --- Spurious IRQ: an unbound line is masked + counted, never dropped -------
    void t_irq_spurious()
    {
        constexpr int FREE_LINE = KICKOS_SELFTEST_IRQ_BASE + 3; // no driver bound to this line
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

    // --- First-arm discards pre-claim garbage ----------------------------------
    // A raise that lands before a driver owns the line is latched, and the FIRST ARM must
    // DISCARD that stale latch (arch_irq_clear_pending), or the very first irq.wait()
    // phantom-wakes on garbage. The arm happens in that first wait, not at claim time, so
    // this is the ONE tier-1 arm where root must NOT pre-arm the line: pre-arming moves
    // the discard into root and stops testing the driver's own first arm.
    int g_stale_seen = 0;
    constexpr int STALE_LINE = KICKOS_SELFTEST_IRQ_BASE + 6;

    void stale_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
        irq.wait();             // first arm discards the latch, then MUST block
        g_stale_seen++;                           // only after root injects a REAL event
        kos_sem_post(CH_DONE);
    }
    void t_irq_stale_register()
    {
        kos_sem_create(0, &g_irq_ready);
        g_stale_seen = 0;
        // Pre-registration garbage on the unbound line: unmask so the default handler
        // runs (mask + count) on the first raise, then a second raise latches on the
        // now-masked line: the stale pending that first-arm must discard.
        kos_irq_unmask(STALE_LINE);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(STALE_LINE); // default handler: mask + count
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        kos_irq_inject(STALE_LINE); // masked now -> latches garbage (pre-claim)
        // Claim but deliberately do NOT arm: the latch must still be there for the
        // driver's own first wait to discard.
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(STALE_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}}; // done@1, ready@2, line@3
        auto drv = kos::thread::spawn_caps(stale_driver, nullptr, "staleirq", 1, caps, 3); // below root
        TAP_CHECK(drv.valid());         // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready); // driver holds the line cap, about to take its first wait
        kos_sem_destroy(g_irq_ready);
        // No phantom: the driver's first wait blocks despite the pre-registration latch.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_stale_seen == 0);
        // Liveness: a real event delivers on the freshly-armed line, driver exits + joins.
        kos_irq_inject(STALE_LINE);
        wait_n(1);
        TAP_CHECK(g_stale_seen == 1);
    }
#endif

    // --- Caller-owned thread stack: spawn takes a caller-provided stack, and rejects an
    // undersized or misaligned one -------------------------------------------------------
    kos_cap_t g_cstk_sem = KOS_CAP_NONE;
    void caller_stack_worker(void*) { kos_sem_post(CH_DONE); } // g_cstk_sem at CH_DONE
    // No-MPU builds ONLY: KOS_STACK_DEFINE aligns to 16 bytes without an MPU, which is what
    // the stack natural-alignment check must tolerate there. Under MPU the macro aligns to a
    // whole region, and the resulting static would not fit a fixed small .appdata window
    // (C6 = 4K); the dynamic stack above covers the MPU case.
#if !KICKOS_HAVE_MPU
    // The worker runs the deepest kernel dispatch (syscall trap frame + thread-exit
    // teardown) on THIS stack, so the size must clear the per-arch KICKOS_MIN_STACK_SIZE
    // floor, which is sized to exactly that dispatch.
    KOS_STACK_DEFINE(g_cstk_static, 2048);
#endif
    void t_caller_stack()
    {
        // Reject a non-null, tiny + misaligned caller stack: -KOS_EINVAL, not run or corrupt.
        TAP_CHECK(kos::thread::spawn(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8).error()
                  == -KOS_EINVAL);
        // Accept a properly-sized, aligned caller-owned stack -> the thread runs on it.
        // When the arena cannot spare one, the reject case above has already run, so the
        // arm stays `ok` but must say which half it dropped.
        constexpr uint32_t STK = 2048;
        void* raw = kos_ram_alloc(STK + 16);
        if (raw == nullptr)
        {
            tap::partial("accept half not run (arena cannot spare a stack)");
            return;
        }
        void* stk = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
        // Reject a PROPERLY-ALIGNED but sub-floor caller stack: without the per-arch floor
        // an aligned 512 B stack passes alignment yet overflows the RISC-V exit dispatch
        // (~624 B). One KICKOS_STACK_ALIGN unit below the floor with an aligned base, so
        // the size check must reject it BEFORE any slot or region work.
        TAP_CHECK(kos::thread::spawn(caller_stack_worker, nullptr, "undf", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, stk, KICKOS_MIN_STACK_SIZE - 16u,
                                     nullptr, 0, nullptr, 0).error()
                  == -KOS_EINVAL);
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant caps[] = {{g_cstk_sem, CH_FULL}}; // -> g_cstk_sem @1 (CH_DONE)
        auto const t = kos::thread::spawn(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                          0, false, nullptr, 0, stk, STK, nullptr, 0, caps, 1);
        TAP_CHECK(t.valid());        // spawn accepted the caller-owned stack
        kos_sem_wait(g_cstk_sem); // the worker ran on it and posted
        kos_sem_destroy(g_cstk_sem);
#if !KICKOS_HAVE_MPU
        // This buffer is only 16-byte aligned (no MPU), and spawn must still accept and run
        // it: with no region descriptor the natural-alignment check must not apply.
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant scaps[] = {{g_cstk_sem, CH_FULL}}; // -> g_cstk_sem @1 (CH_DONE)
        auto const ts = kos::thread::spawn(caller_stack_worker, nullptr, "cstkS", 10, KOS_POLICY_FIFO,
                                           0, false, nullptr, 0, g_cstk_static,
                                           static_cast<uint32_t>(sizeof(g_cstk_static)),
                                           nullptr, 0, scaps, 1);
        TAP_CHECK(ts.valid()); // spawn accepted the static caller-owned stack
        kos_sem_wait(g_cstk_sem); // the worker ran on it and posted
        kos_sem_destroy(g_cstk_sem);
#endif
    }

    // --- Memory domains: two unprivileged threads granted the SAME region share one
    // domain, each reading/writing it, while each keeps its own private stack. The
    // negative half (a cross-domain write faults) is the standalone mpu_fault app. ---
    volatile int* g_dshared = nullptr;
    kos_cap_t g_dwrote = KOS_CAP_NONE; // writer -> reader handoff (through the shared domain)
    kos_cap_t g_dread = KOS_CAP_NONE;  // reader -> main handoff
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
        // Nothing to assert on a part whose arena cannot spare the region: a real SKIP.
        // Alloc before the sems so an early return leaks nothing.
        g_dshared = static_cast<volatile int*>(kos_ram_alloc(256));
        if (g_dshared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // No pre-zero of the region: g_dreadback is written only by the reader, after
        // the writer's post, so a stale word cannot fake the sentinel. An unprivileged
        // root could not write the region anyway (allocated, never granted).
        kos_sem_create(0, &g_dwrote);
        kos_sem_create(0, &g_dread);
        // Spawn BOTH before either runs (spawn does not preempt): same mem_base =>
        // they reference the ONE shared domain concurrently, each with its own stack.
        kos_cap_grant wcaps[] = {{g_dwrote, CH_FULL}};                    // g_dwrote@1
        kos_cap_grant rcaps[] = {{g_dwrote, CH_FULL}, {g_dread, CH_FULL}}; // g_dwrote@1, g_dread@2
        auto w = kos::thread::spawn_caps(dom_writer, nullptr, "domW", 10, wcaps, 1, KOS_POLICY_FIFO,
                                         0, false, const_cast<int*>(g_dshared), 256);
        auto r = kos::thread::spawn_caps(dom_reader, nullptr, "domR", 10, rcaps, 2, KOS_POLICY_FIFO,
                                         0, false, const_cast<int*>(g_dshared), 256);
        if (not w.valid() or not r.valid())
        {
            // A 2-slot pool with a low-prio driver from an earlier arm still parked cannot
            // host both workers concurrently. sim and qemu have the pool to exercise it.
            // Whichever worker did spawn self-completes, so nothing needs draining.
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

    // --- MMIO grant boundary: privileged-only + encodable-only -------------------
    // No real device is mapped here; the positive grant is HW-only. The boundary must
    // REJECT two ways:
    //   * a window one MPU descriptor cannot cover exactly (rounding would over-grant
    //     the neighbouring registers), and
    //   * any grant attempted by an UNPRIVILEGED caller (else a user thread maps
    //     arbitrary peripheral space and defeats isolation).
    // The sim's arch_mpu_region_encodable admits ONE window (its fake register block,
    // t_periph_reg_write_mask) and neither of these names it, so both halves still land
    // as a -1 spawn there; on an enforcing MCU the first rejects the non-encodable
    // window and the second the privilege violation.
    int g_mmio_unpriv_rc = -2;
    kos_cap_t g_mmio_done = KOS_CAP_NONE;
    void mmio_noop(void*) {}
    void mmio_unpriv_worker(void*)
    {
        // Unprivileged caller: the privilege gate must refuse the MMIO grant.
        g_mmio_unpriv_rc = kos::thread::spawn(mmio_noop, nullptr, "mmiochild", 10,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                              nullptr, 0,
                                              reinterpret_cast<void*>(0x1000u), 4096)
                               .error();
        kos_sem_post(CH_DONE); // g_mmio_done
    }
    void t_mmio_grant()
    {
        // Non-encodable window (size 1, unaligned base): rejected with -KOS_EINVAL, not
        // rounded. Geometry is checked ahead of the privilege gate, so the code is
        // -KOS_EINVAL whatever posture the caller runs in.
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmiobad", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x1001u), 1).error() == -KOS_EINVAL);
        // A non-null base with size 0 is rejected at the boundary (before domain_for).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmio0", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0x2000u), 0).error() == -KOS_EINVAL);
        // A window whose base+size wraps the address space is rejected -KOS_EINVAL (32-bit
        // MCU; on the 64-bit sim the fail-closed encoder rejects it first, either way EINVAL).
        TAP_CHECK(kos::thread::spawn(mmio_noop, nullptr, "mmioW2", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(0xFFFFFFF0u), 0x20).error() == -KOS_EINVAL);
        kos_sem_create(0, &g_mmio_done);
        g_mmio_unpriv_rc = -2;
        kos_cap_grant caps[] = {{g_mmio_done, CH_FULL}}; // g_mmio_done@1 (CH_DONE)
        auto w = kos::thread::spawn_caps(mmio_unpriv_worker, nullptr, "mmioW", 10, caps, 1);
        if (not w.valid())
        {
            // Tiny thread pool (e.g. microbit MAX_THREADS=2). The three encodability
            // cases above already ran, so this stays `ok` and names the dropped half.
            tap::partial("unprivileged half not run (thread pool too small)");
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
    // kernel SRAM: an R|W window the MMIO gate would refuse. Enforcing backends only
    // (the check is MPU-gated; with no region descriptor there is no escalation).
#if KICKOS_HAVE_MPU
    int g_stkarena_rc = -2;
    kos_cap_t g_stkarena_done = KOS_CAP_NONE;
    void stkarena_noop(void*) {}
    void stkarena_unpriv_worker(void*)
    {
        // Unprivileged caller; stack_base far above any SRAM arena and naturally aligned
        // (clears the size/align/natural checks) so ONLY the arena bound can reject it.
        g_stkarena_rc = kos::thread::spawn(stkarena_noop, nullptr, "stkbad", 10,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                           reinterpret_cast<void*>(0xE0000000u), 2048)
                            .error();
        kos_sem_post(CH_DONE); // g_stkarena_done
    }
    void t_stackbase_arena()
    {
        kos_sem_create(0, &g_stkarena_done);
        g_stkarena_rc = -2;
        kos_cap_grant caps[] = {{g_stkarena_done, CH_FULL}}; // g_stkarena_done@1 (CH_DONE)
        auto w = kos::thread::spawn_caps(stkarena_unpriv_worker, nullptr, "stkW", 10, caps, 1);
        if (not w.valid())
        {
            // The unprivileged child IS this arm, so with no thread there is nothing to
            // assert: a whole-arm SKIP, never a partial.
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
    // Exercises grant_hits_reserved / grant_region_admissible through kos_grant_probe. The
    // reserved-OVERLAP matrix needs a board that declares reserved blocks; the sim reserves
    // nothing, so that half reports PARTIAL there and runs for real on an enforcing MCU.
    // The bit-band alias-hit case needs a bit-band M4 and is HW-only.
    void grant_noop(void*) {}
    void t_grant_reserved()
    {
        // --- RAM-path admission (arena-relative; runs on sim). ---
        // kos_ram_alloc hands back a block the arch can name with one descriptor, so it is
        // admissible R|W for EVERY caller posture. The probed size must be a granule
        // multiple; the sim's granule is a 4 KiB host page.
        size_t const g = discover_granule();
        void* raw = nullptr;
        if (g != 0)
        {
            raw = kos_ram_alloc(g);
        }
        if (raw != nullptr)
        {
            uintptr_t const a = reinterpret_cast<uintptr_t>(raw);
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a, g) == 1);      // in-arena, encodable, privileged
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_UNPRIVILEGED, a, g) == 1);    // in-arena, encodable, unprivileged
            // a + 1 is sub-granule on every backend: the smallest granule in the tree
            // is PMP's 8.
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a + 1, g) == 0);  // R1: base below the arch's region granule
        }
        else
        {
            tap::partial("arena-relative RAM cases not run (granule alloc failed)");
        }
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x1000u, 0x1000u) == 0);      // out-of-arena RAM refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0xFFFFFFF0u, 0x20u) == 0);    // wrap (32-bit) / out-of-arena (64-bit) refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x20000000u, 0u) == 0);       // size 0 refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_UNPRIVILEGED, 0x40000000u, 0x1000u) == 0);  // DEV grant, unprivileged caller: refused

        // --- End-to-end errno coherence: an unprivileged child whose mem_base lies OUTSIDE
        // the arena is refused with -KOS_EPERM (policy refusal), NOT -KOS_ENOMEM (pool
        // exhaustion), matching the stack_base path in t_stackbase_arena. The code must come
        // from domain_for, not a pre-check at the spawn boundary. 0xE0000000 is 2048-aligned,
        // so ONLY arena containment can reject it. Fails before any slot is claimed.
        auto const mrc = kos::thread::spawn(grant_noop, nullptr, "membad", 10, KOS_POLICY_FIFO,
                                            0, /*privileged=*/false,
                                            reinterpret_cast<void*>(0xE0000000u), 2048);
        TAP_CHECK(mrc.error() == -KOS_EPERM); // out-of-arena data grant: policy refusal, never ENOMEM

        // --- Reserved-overlap matrix. ---
        // The OVERLAP cases anchor on block[0] and are layout-independent (a window that
        // overlaps block[0] hits regardless of neighbours). The NON-overlap cases must
        // land in a GAP, so they anchor on scanned edges: the lowest reserved base has
        // nothing flush below it, and the highest reserved end has nothing at-or-above
        // it, guaranteed by min/max even when blocks are flush (rp2040 TIMER abuts
        // WATCHDOG). Testing block[0]-relative "above" would false-hit on such a board.
        uintptr_t const n = kos_grant_probe(KOS_GRANT_OP_RESERVED_COUNT, 0, 0);
        if (n == 0)
        {
            tap::partial("reserved-overlap matrix not run (board reserves nothing)");
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
        // Refuse (overlap): equal, contained, partial straddle, both one-byte edges.
        // All overlap block[0], so hit regardless of layout.
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
        auto const rc = kos::thread::spawn(grant_noop, nullptr, "rsvd", 10, KOS_POLICY_FIFO,
                                           0, false, nullptr, 0, nullptr, 0,
                                           reinterpret_cast<void*>(rb),
                                           static_cast<uint32_t>(rs));
        TAP_CHECK(not rc.valid()); // reserved-block MMIO grant refused (domain_for, or non-encodable at the boundary)
    }

    // --- One holder per device window (-KOS_EBUSY) --------------------------------
    // A DEV window overlapping one a LIVE domain already holds is refused. Matched on
    // RANGES, so this covers all three shapes in one case: exact duplicate and partial
    // overlap refuse, adjacent-but-disjoint admits.
    //
    // The holder must stay ALIVE for the whole matrix, and that cannot rest on it not
    // being scheduled: an interrupt between two spawns reschedules, the prio-10 holder
    // runs to its return and exit_current frees the window, turning every refusal below
    // into an admission. It parks on a semaphore instead; root reclaims the window by
    // posting it, never by timing.
    //
    // The window is DISCOVERED, not hardcoded: kos_grant_probe says which bases the DEV
    // predicate admits on this board (reserved blocks and bit-band aliases differ per
    // chip), and the holder spawn itself rejects a base a board service already owns.
    // The sim admits exactly one DEV window, its fake register block at SIM_PVREG_WINDOW
    // (64 KiB), so a WIN-sized search finds nothing there and the case reports PARTIAL. On
    // any other enforcing board, the qemu base variant included, an empty search
    // must FAIL.
    constexpr int CH_DEVHOLD = 2; // the holder gate, delegated SECOND (done@1, hold@2)
    void devexcl_hold(void*) // caps: done@1, hold@2
    {
        kos_sem_wait(CH_DEVHOLD); // hold the window until root releases it
        kos_sem_post(CH_DONE);
    }
    void t_dev_window_exclusive()
    {
        constexpr uint32_t WIN = 0x100u; // pow2 >= 32: encodable on PMSAv7/v8 and byte-granular SYSMPU
        // Step 2*WIN so both `base` and its adjacent sibling `base + WIN` stay
        // WIN-aligned (PMSA needs natural alignment, so an unaligned base is not
        // merely refused-as-held but refused-as-unencodable).
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_sem_create(0, &hold) != 0)
        {
            tap::fail("no semaphore for the holder gate; exclusivity cannot be staged");
            return;
        }
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {hold, CH_FULL}}; // -> done@1, hold@2
        uintptr_t win = 0;
        kos::thread::Handle holder;
        int live = 0; // parked holders root still owes a post
        bool any_admissible = false;
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 2u * WIN)
        {
            if (kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, b, WIN) != 1
                or kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, b + WIN, WIN) != 1)
            {
                continue; // reserved / alias / non-encodable on this chip
            }
            any_admissible = true;
            holder = kos::thread::spawn(devexcl_hold, nullptr, "devheld", 10, KOS_POLICY_FIFO,
                                        0, /*privileged=*/false, nullptr, 0, nullptr, 0,
                                        reinterpret_cast<void*>(b), WIN, hcaps, 2);
            if (holder.valid())
            {
                live++;
                win = b;
                break;
            }
            if (holder.error() != -KOS_EBUSY)
            {
                break; // not "a board service owns this window": a real refusal, report it
            }
        }
        if (not any_admissible)
        {
            // Positively the sim, so a new arch with no DEV encoder fails loudly here
            // instead of inheriting this escape.
#if KICKOS_ARCH_SIM
            // The sim admits exactly ONE DEV window shape (64 KiB), never a WIN-sized one,
            // so the exclusivity matrix needs two windows the sim cannot both mint. Assert
            // THAT premise instead of skipping: the sim gate reads a skip as "an arm stopped
            // running" (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            TAP_CHECK(not kos::thread::spawn(devexcl_hold, nullptr, "devnone", 10,
                                             KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                             reinterpret_cast<void*>(0x40000000u), WIN,
                                             hcaps, 2)
                              .valid());
            tap::partial("board mints no DEV window; exclusivity runs on enforcing boards "
                         "(e.g. the qemu base variant)");
#else
            // An enforcing board with no admissible DEV base means kos_grant_probe or the
            // discovery loop regressed. Passing here would silently drop every -KOS_EBUSY
            // assertion below while the gate stayed green.
            tap::fail("no DEV-admissible window in [0x40000000, 0x40100000) -- "
                      "kos_grant_probe or window discovery regressed");
#endif
            kos_sem_destroy(hold);
            return;
        }
        if (win == 0)
        {
            kos_sem_destroy(hold);
            if (holder.error() != -KOS_EBUSY)
            {
                // A refusal that is not "a board service owns this window": EPERM/EINVAL
                // here is a boundary regression, not an unrunnable case.
                tap::fail("DEV holder spawn refused rc %d, expected 0 or -KOS_EBUSY",
                          holder.error());
                return;
            }
            tap::skip("every DEV-admissible window is already held (holder rc %d)",
                      holder.error());
            return;
        }
        // 1. EXACT DUPLICATE of the live holder's window: refused, and specifically
        //    EBUSY, not EPERM (the window is admissible) and not ENOMEM (the pool has
        //    room; the refusal lands before a slot is claimed).
        TAP_CHECK(kos::thread::spawn(devexcl_hold, nullptr, "devdup", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(win), WIN, hcaps, 2).error() == -KOS_EBUSY);
        // 2. PARTIAL overlap: the upper half of the held window. Its own base/size are
        //    independently admissible, so only the overlap scan can refuse it.
        TAP_CHECK(kos::thread::spawn(devexcl_hold, nullptr, "devpart", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, nullptr, 0,
                                     reinterpret_cast<void*>(win + WIN / 2u), WIN / 2u, hcaps, 2).error()
                  == -KOS_EBUSY);
        // 3. ADJACENT but disjoint (base == held last + 1): ADMITTED. This is the mk64f
        //    PIT CH2 shape: a grant flush against a block, which must not be read as
        //    overlapping. It proves the scan did not widen adjacency into overlap.
        auto const adj = kos::thread::spawn(devexcl_hold, nullptr, "devadj", 10, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, nullptr, 0,
                                            reinterpret_cast<void*>(win + WIN), WIN, hcaps, 2);
        TAP_CHECK(adj.valid()); // adjacency is not overlap
        if (adj.valid())
        {
            live++;
        }
        // The post is what frees the windows, so the drain does not depend on timing.
        for (int i = 0; i < live; i++)
        {
            kos_sem_post(hold);
        }
        wait_n(live);
        // The window is free again once the holder is gone, so the SAME grant that was
        // refused above now succeeds: proving the refusal tracked live holders, not the
        // address. CH_DONE is posted before the holder returns, so retry the grant
        // rather than assume the domain is already released.
        int again = -KOS_EBUSY;
        for (int i = 0; i < 100 and again == -KOS_EBUSY; i++)
        {
            again = kos::thread::spawn(devexcl_hold, nullptr, "devagain", 10, KOS_POLICY_FIFO,
                                       0, false, nullptr, 0, nullptr, 0,
                                       reinterpret_cast<void*>(win), WIN, hcaps, 2)
                        .error();
            if (again == -KOS_EBUSY)
            {
                kos_sleep_ns(1000000ull); // 1 ms
            }
        }
        TAP_CHECK(again == 0); // holder exited -> window released
        if (again == 0)
        {
            kos_sem_post(hold); // drain it too, so later tests get the slot back
            wait_n(1);
        }
        kos_sem_destroy(hold);
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
    // The positive half proves only that the floor ACCEPTED the pointer, not that bytes
    // reached a console: kos_kconsole_write returns `len` even when console_emit drops it
    // all. It is non-vacuous only when PAIRED with the guard-page negative below; with no
    // guard page it stands alone and proves much less.
    char const CD_LIT[] = "# [confdep] unpriv rodata buffer accepted by the readable floor\n";
    long g_cd_lit_rc = -99;    // worker: kconsole_write(rodata literal) -> expect len (accepted, not delivered)
    int g_cd_goodspawn = -99;  // worker: spawn rc of a child NAMED from .rodata
    int g_cd_goodname_ran = 0; // that child ran (name-copy path did not break spawn)
    kos_cap_t g_cd_kidsem = KOS_CAP_NONE; // grandchild -> worker handoff
    kos_cap_t g_cd_done = KOS_CAP_NONE;   // worker -> main
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    int g_cd_neg_ran = 0;       // the negative half actually ran (guard page available)
    long g_cd_bad_rc = -99;     // worker: kconsole_write(guard page) -> expect 0 (rejected)
    int g_cd_badname_spawn = -99; // spawn rc with a BOGUS name pointer -> expect 0
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
        // it to a grandchild: nested delegation requires the source cap carry TRANSFER,
        // which sem_create grants. g_cd_kidsem is cd_worker's cap value (its table).
        kos_sem_create(0, &g_cd_kidsem);
        kos_cap_grant kidcaps[] = {{g_cd_kidsem, CH_FULL}}; // -> grandchild's index 1
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        g_cd_goodspawn = kos::thread::spawn_caps(cd_kid, &g_cd_goodname_ran, "cdgood", 9,
                                                 kidcaps, 1)
                             .error();
        if (g_cd_goodspawn == 0)
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
                                                         kidcaps, 1)
                                     .error();
            if (g_cd_badname_spawn == 0)
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
        kos_sem_create(0, &g_cd_done);
        kos_cap_grant caps[] = {{g_cd_done, CH_FULL}}; // g_cd_done@1 (CH_DONE)
        auto w = kos::thread::spawn_caps(cd_worker, nullptr, "cdwork", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_cd_done);
            return;
        }
        kos_sem_wait(g_cd_done);
        kos_sem_destroy(g_cd_done);
        // Positive (every backend): the floor accepted an unprivileged caller's rodata
        // pointer and read exactly len bytes. See CD_LIT: acceptance, not delivery.
        TAP_CHECK(g_cd_lit_rc == static_cast<long>(sizeof(CD_LIT) - 1));
        // The grandchild needs its own stack, and on a 16 KiB-SRAM part that alloc can fail.
        // The rodata-literal positive above already covered the read path, so drop this half
        // rather than fail; sim and qemu still cover the name-copy path.
        if (g_cd_goodspawn != 0)
        {
            tap::partial("grandchild-name half not run (arena too small for its stack)");
            return;
        }
        TAP_CHECK(g_cd_goodname_ran == 1);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        // Negative (enforcing backend): a bogus buffer/name is rejected, never read,
        // and never faults the kernel.
        if (g_cd_neg_ran)
        {
            TAP_CHECK(g_cd_bad_rc == -KOS_EFAULT); // bogus buffer rejected, never read (was 0)
            TAP_CHECK(g_cd_badname_spawn == 0 and g_cd_badname_ran == 1);
        }
#endif
    }

    // --- Endpoint IPC: synchronous rendezvous send/recv ----------
    // The endpoint cap is delegated to workers at child index 2 (done@1, E@2). Workers
    // are UNPRIVILEGED so the kernel's copy into/from a parked peer runs against real
    // enforcement (the cross-domain privileged write, design section 3.1).
    char const EP_MSG[] = "hello-endpoint"; // 14 bytes (strlen), no NUL sent
    constexpr uint8_t EP_SIGNAL_ONLY = KOS_CAP_SIGNAL; // send right only
    constexpr uint8_t EP_WAIT_ONLY = KOS_CAP_WAIT;     // recv right only
    kos_cap_t g_ep = KOS_CAP_NONE; // main's endpoint cap (created per test)
    char g_ep_rbuf[64];
    Atomic<int32_t, Order::RELAXED> g_ep_rn{-99};         // worker recv return
    Atomic<uint32_t, Order::RELAXED> g_ep_rbadge{0xffffffffu};
    Atomic<int, Order::RELAXED> g_ep_rcap{64}; // capacity the recv worker passes
    Atomic<int32_t, Order::RELAXED> g_ep_sn{-99}; // worker send return

    void ep_recv_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        // Keep the recv buffer thread-private: a global one is also accepted
        // (user_writable_ok has a static-data fallback, covered by writable_global) and
        // would make this arm about the writable check instead of the rendezvous.
        char buf[64];
        struct kos_recv_info info = {0xdeadu, 0x55};
        int32_t n = kos_recv(2, buf, static_cast<size_t>(g_ep_rcap), &info);
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}}; // done@1, E@2

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rcap = 64;
        auto w = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eprx", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull); // let the worker park in recv
        int32_t sc = kos_send(g_ep, EP_MSG, mlen);
        TAP_CHECK(sc == static_cast<int32_t>(mlen)); // sender delivered n bytes
        wait_n(1);
        int32_t const ep_rn_a = g_ep_rn;
        TAP_CHECK(ep_rn_a == static_cast<int32_t>(mlen) and memcmp(g_ep_rbuf, EP_MSG, mlen) == 0);
        uint32_t const ep_rbadge = g_ep_rbadge;
        TAP_CHECK(ep_rbadge == 0); // badge always written on success (stage i: 0)

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        auto w2 = kos::thread::spawn_caps(ep_send_worker, nullptr, "eptx", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w2.valid());
        kos_sleep_ns(3000000ull); // let the worker park in send
        char rbuf[64];
        struct kos_recv_info info = {0xdeadu, 0x55};
        int32_t rc = kos_recv(g_ep, rbuf, sizeof(rbuf), &info);
        TAP_CHECK(rc == static_cast<int32_t>(mlen) and memcmp(rbuf, EP_MSG, mlen) == 0);
        TAP_CHECK(info.badge == 0);
        wait_n(1);
        int32_t const ep_sn = g_ep_sn;
        TAP_CHECK(ep_sn == static_cast<int32_t>(mlen));

        // (C) zero-length is a valid signal (n == 0 on both sides, NOT -1).
        g_ep_rn = -99; g_ep_rcap = 64;
        auto w3 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "epz", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w3.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, 0) == 0);
        wait_n(1);
        int32_t const ep_rn_c = g_ep_rn;
        TAP_CHECK(ep_rn_c == 0);

        // (D) truncation: send mlen into a 4-byte capacity -> both return 4.
        g_ep_rn = -99; g_ep_rcap = 4;
        auto w4 = kos::thread::spawn_caps(ep_recv_worker, nullptr, "eptr", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w4.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, mlen) == 4);
        wait_n(1);
        int32_t const ep_rn_d = g_ep_rn;
        TAP_CHECK(ep_rn_d == 4 and memcmp(g_ep_rbuf, EP_MSG, 4) == 0);

        TAP_CHECK(kos_handle_close(g_ep) == 0); // last cap -> endpoint freed
    }

    // --- Oversize reject + bad cap (main only; no parking) -----------------------
    void t_endpoint_reject()
    {
        char big[KOS_EP_MSG_MAX + 8];
        memset(big, 'x', sizeof(big));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
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
    Atomic<int, Order::RELAXED> g_ep_wait_send_rc{-99};   // WAIT-only cap send -> -1
    Atomic<int, Order::RELAXED> g_ep_signal_recv_rc{-99}; // SIGNAL-only cap recv -> -1
    void ep_rights_worker(void*) // caps: done@1, E(WAIT)@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        g_ep_wait_send_rc = static_cast<int>(kos_send(2, b, 1)); // WAIT-only -> no SIGNAL -> -KOS_EPERM
        g_ep_signal_recv_rc = static_cast<int>(kos_recv(3, b, sizeof(b), nullptr)); // no WAIT -> -KOS_EPERM
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_rights()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_wait_send_rc = -99; g_ep_signal_recv_rc = -99;
        // Two narrowed caps to the same endpoint: WAIT-only at index 2, SIGNAL-only at 3.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::spawn_caps(ep_rights_worker, nullptr, "eprt", 12, caps, 3,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        wait_n(1);
        int const ep_wait_send_rc = g_ep_wait_send_rc;
        int const ep_signal_recv_rc = g_ep_signal_recv_rc;
        TAP_CHECK(ep_wait_send_rc == -KOS_EPERM);   // send refused without SIGNAL
        TAP_CHECK(ep_signal_recv_rc == -KOS_EPERM); // recv refused without WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- EPIPE: a parked sender is woken -1 when the last WAIT-cap holder drops it -
    // A SIGNAL-only delegation does NOT bump recv_holders, so main's cap is the sole
    // WAIT holder: closing it takes recv_holders 1->0 and EPIPEs the parked sender.
    Atomic<int32_t, Order::RELAXED> g_ep_epipe_rc{-99};
    void ep_epipe_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_ep_epipe_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks; woken -KOS_EPIPE on EPIPE
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_epipe()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_epipe_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        auto w = kos::thread::spawn_caps(ep_epipe_worker, nullptr, "epep", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull);              // let the sender park (recv_holders == 1 == main)
        TAP_CHECK(kos_handle_close(g_ep) == 0); // last WAIT cap -> EPIPE the parked sender
        wait_n(1);
        int32_t const ep_epipe_rc = g_ep_epipe_rc;
        TAP_CHECK(ep_epipe_rc == -KOS_EPIPE); // sender woken with EPIPE, not a byte count
    }

    // --- Dead endpoint (unparked): send after the last WAIT cap is gone -> -1 -----
    // Distinct from the parked-then-EPIPE case: the sender never parks (F1 dead-check).
    Atomic<int32_t, Order::RELAXED> g_ep_dead_rc{-99};
    kos_cap_t g_ep_go = KOS_CAP_NONE;
    void ep_dead_worker(void*) // caps: done@1, E(SIGNAL)@2, go@3
    {
        kos_sem_wait(3);                                     // go: main has dropped its WAIT cap
        g_ep_dead_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // recv_holders == 0 -> -KOS_EPIPE now
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_dead()
    {
        int const eprc = kos_endpoint_create(&g_ep);
        int const gorc = kos_sem_create(0, &g_ep_go);
        TAP_CHECK(eprc == 0 and gorc == 0);
        g_ep_dead_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}, {g_ep_go, CH_FULL}};
        auto w = kos::thread::spawn_caps(ep_dead_worker, nullptr, "epde", 12, caps, 3,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        // Close main's (only) WAIT cap FIRST: recv_holders -> 0, no sender parked yet.
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        kos_sem_post(g_ep_go); // now the worker sends into the dead endpoint
        wait_n(1);
        int32_t const ep_dead_rc = g_ep_dead_rc;
        TAP_CHECK(ep_dead_rc == -KOS_EPIPE); // dead endpoint rejected immediately, never parked
        kos_sem_destroy(g_ep_go);
    }

    // --- Timed send: the deadline expires with a LIVE endpoint and nobody in recv ------
    // Main keeps its WAIT cap for the whole arm, so recv_holders stays 1 and no EPIPE can
    // fire: the only thing missing is a parked receiver, which is exactly what a deadline
    // has to answer for. The worker's report rides an UNTIMED send on the SAME endpoint,
    // and main recvs it only after sleeping far past the deadline, so the report arriving
    // at all is the proof that the three-argument form still parks.
    constexpr uint32_t EP_SEND_TIMEOUT_US = 4000;
    // 20x the deadline, not 6x: this sleep must still be running when the deadline fires,
    // so the margin has to absorb a timer that overruns.
    constexpr uint64_t EP_SEND_TIMEOUT_WAIT_NS = 80000000ull;
    // `entered` is the clock immediately before the timed syscall. It is what lets a
    // receiving arm decide WHICH path it staged: a reading earlier than the receiver's own
    // pre-syscall reading means the caller was already parked (slow path), a later one
    // means the receiver parked first (fast path). Both stagings satisfy every rc and
    // duration assertion, so nothing else here can tell them apart.
    struct EpTimedSend
    {
        int32_t rc;
        uint32_t waited_us;
        uint64_t entered;
    };
    void ep_timed_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        EpTimedSend r;
        uint64_t const t0 = kos_clock_now();
        r.entered = t0;
        r.rc = static_cast<int32_t>(
            kos_send_timed(2, EP_MSG, strlen(EP_MSG), EP_SEND_TIMEOUT_US));
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        (void)kos_send(2, &r, sizeof(r)); // untimed: parks until main's recv, long after
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_send_timeout()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        auto w = kos::thread::spawn_caps(ep_timed_worker, nullptr, "eptm", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(EP_SEND_TIMEOUT_WAIT_NS); // outlast the deadline without ever recv'ing
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        int32_t const n = kos_recv(g_ep, &r, sizeof(r), nullptr);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired, and no bytes crossed
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_SEND_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // The three timed-call arms share one deadline and one outlast-it sleep. Main keeps its
    // WAIT cap for the whole of each, so recv_holders stays 1 and no EPIPE can be mistaken
    // for an expiry. Each caller reports over an UNTIMED send on its own endpoint, so no
    // arm needs file-scope result state.
    constexpr uint32_t EP_CALL_TIMEOUT_US = 4000;
    // 20x, for the same reason as EP_SEND_TIMEOUT_WAIT_NS.
    constexpr uint64_t EP_CALL_TIMEOUT_WAIT_NS = 80000000ull;
    constexpr uint64_t EP_CALL_SETTLE_NS = 3000000ull; // long enough for the caller to park
    // The reply-wait arm needs the OPPOSITE ordering from every other arm here: the server
    // must pop the caller BEFORE its deadline fires, so the settle sleep has to finish
    // inside the deadline rather than outlast it. The margin is 20x, not the 1x a 4 ms
    // deadline would leave: at 1x the caller times out first, the server's recv returns the
    // report instead of the request, and the arm leaks its reply cap into the census
    // cap_child_width reads.
    constexpr uint32_t EP_CALL_REPLY_TIMEOUT_US = 60000;
    constexpr uint64_t EP_CALL_REPLY_WAIT_NS = 1200000000ull; // 20x the deadline above
    // A deadline no arm here can reach: it is on a recv that pops an ALREADY-parked peer,
    // so it never arms, and it doubles as the witness that the kernel leaves the input word
    // alone.
    constexpr uint32_t EP_RECV_GENEROUS_US = 200000;

    // --- Timed call: the deadline expires while parked on send_waiters ------------------
    // The endpoint HAS a conventional server (main's warm-up recv seats ep->server) but
    // nobody is in recv when the call lands, so the caller parks as CALL_SEND_WAIT and the
    // unwind has a real D2 boost to revert rather than the null-server shortcut.
    void ep_call_pending_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        char warm[1] = {0};
        kos_send(2, warm, sizeof(warm)); // main's warm-up recv takes this and seats ep->server
        char buf[8] = {0};
        EpTimedSend r;
        uint64_t const t0 = kos_clock_now();
        r.entered = t0;
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        (void)kos_send(2, &r, sizeof(r)); // untimed: parks until main's recv, long after
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_pending()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        auto w = kos::thread::spawn_caps(ep_call_pending_worker, nullptr, "cltp", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        char warm[1] = {0};
        TAP_CHECK(kos_recv(g_ep, warm, sizeof(warm), nullptr) == 1); // seats ep->server = main
        kos_sleep_ns(EP_CALL_TIMEOUT_WAIT_NS); // outlast the deadline without ever recv'ing
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        int32_t const n = kos_recv(g_ep, &r, sizeof(r), nullptr);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired on send_waiters, never taken
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Timed call: the deadline expires in CALL_REPLY_WAIT, reached by the SLOW path ---
    // THE arm for the one-deadline-spans-both-phases rule. The caller parks on send_waiters
    // first (main sleeps instead of recv'ing), and main's info-bearing recv then MIGRATES it
    // onto main's reply_waiters. That migration is a park-to-park move and not an unpark, so
    // the deadline armed once at the call must survive it: were the cancel back in
    // wq_pop_highest, this caller would park forever and the arm would HANG rather than
    // fail. Main never replies.
    //
    // The slow path is the whole point, and no assertion below enforces it: staged on the
    // fast path the deadline is armed straight onto the reply park, nothing migrates, and
    // every rc, duration and cap assertion below still passes. `r.entered` separates them.
    void ep_call_reply_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        EpTimedSend r;
        uint64_t const t0 = kos_clock_now();
        r.entered = t0;
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_REPLY_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_reply()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::spawn_caps(ep_call_reply_worker, nullptr, "cltr", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        // Root is the lowest-priority thread, so once this sleep blocks root the caller
        // runs to its own park before root can resume.
        kos_sleep_ns(EP_CALL_SETTLE_NS); // the caller is now parked as CALL_SEND_WAIT
        char req[8];
        // A TIMED recv here, generously bounded: the caller is already parked, so this pops
        // it without ever arming a deadline, and the surviving opts.timeout_us is what shows
        // the kernel wrote the NESTED kos_recv_info and left the input word alone. A loop
        // reusing one opts struct depends on exactly that.
        struct kos_recv_timed_opts opts;
        opts.timeout_us = EP_RECV_GENEROUS_US;
        opts.info.badge = 0;
        opts.info.reply_cap = KOS_CAP_NONE;
        uint64_t const before_recv = kos_clock_now();
        int32_t const got = kos_recv_timed(g_ep, req, sizeof(req), &opts); // slow-path pop + migration
        TAP_CHECK(got == 4);
        TAP_CHECK(opts.info.reply_cap != KOS_CAP_NONE); // we hold the reply cap, and never use it
        TAP_CHECK(opts.timeout_us == EP_RECV_GENEROUS_US);
        kos_cap_t const reply_cap = opts.info.reply_cap;
        kos_sleep_ns(EP_CALL_REPLY_WAIT_NS); // outlast the deadline without replying
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        int32_t const n = kos_recv(g_ep, &r, sizeof(r), nullptr);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        // THE staging witness. The caller read its clock before entering the call; that
        // reading precedes main's own pre-recv reading, and main was running when it took
        // that reading, so the caller was already off-CPU inside the call. The recv above
        // therefore POPPED a parked caller instead of parking. A fast-path staging inverts
        // this and fails here rather than passing on the path the arm does not test.
        TAP_CHECK(r.entered < before_recv);
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // the deadline crossed the handoff and fired
        TAP_CHECK(r.waited_us >= EP_CALL_REPLY_TIMEOUT_US);
        // The cap outlives the caller by design (reclaiming it would cross a containment
        // boundary), so closing it is still the server's job and must succeed with nobody
        // left to wake.
        TAP_CHECK(kos_handle_close(reply_cap) == 0);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Reply to a caller that already timed out: -KOS_ESRCH, cap consumed --------------
    // Staged on the FAST path (main is already parked in recv when the call lands), which
    // is the other half of the CALL_REPLY_WAIT unwind: there the deadline is armed straight
    // onto the reply park.
    void ep_reply_stale_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        EpTimedSend r;
        // The settle sleep is on the CALLER here, the mirror image of the reply-wait arm:
        // main must reach its recv and park before this call lands, or the call takes the
        // slow path and the reply cap is minted by the recv-side scan instead.
        kos_sleep_ns(EP_CALL_SETTLE_NS);
        uint64_t const t0 = kos_clock_now();
        r.entered = t0;
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }
    void t_reply_stale_caller()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::spawn_caps(ep_reply_stale_worker, nullptr, "rpst", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        char req[8];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        uint64_t const before_recv = kos_clock_now();
        int32_t const got = kos_recv(g_ep, req, sizeof(req), &info); // parks, then the call fills it
        TAP_CHECK(got == 4 and info.reply_cap != KOS_CAP_NONE);
        kos_sleep_ns(EP_CALL_TIMEOUT_WAIT_NS); // the caller's deadline expires under us
        char rep[4] = {0};
        // The cap still resolves, but the caller it names left CALL_REPLY_WAIT, so the
        // reply has nowhere to land. It is consumed anyway.
        TAP_CHECK(kos_reply(info.reply_cap, rep, sizeof(rep)) == -KOS_ESRCH);
        // Consumed exactly once: the slot is empty and its cap-gen rolled, so the handle no
        // longer resolves at all and the second attempt fails EARLIER, on the cap.
        TAP_CHECK(kos_reply(info.reply_cap, rep, sizeof(rep)) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(info.reply_cap) == -KOS_EBADF);
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        int32_t const n = kos_recv(g_ep, &r, sizeof(r), nullptr);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        // The mirror of the reply-wait arm's witness: the caller entered its call AFTER
        // main took the reading above and then stopped running, so main was parked in the
        // recv when the call landed. That is the FAST path, and a slow-path staging fails
        // here instead of duplicating the other arm.
        TAP_CHECK(r.entered > before_recv);
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // expired on the reply park, not on send_waiters
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Reply on an abandoned cap once its sequence has come round again ---------------
    // reply_stale_caller covers a timed-out caller that is doing nothing else: there
    // cap_reply_caller answers nullptr on the call_state test alone, and the sequence never
    // has to be right. This arm reaches PAST that test. The caller times out, leaving a
    // live one-shot cap in a server's table, and then runs further calls until its call_seq
    // low byte comes back to the one packed in that cap. On that call the abandoned cap
    // resolves through index, generation, BLOCKED, CALL_REPLY_WAIT and sequence: every
    // test but the last.
    //
    // What must then stop it is that the caller is parked on the SECOND server's
    // reply_waiters, so the abandoned holder's unlink finds nothing of its own and the
    // reply completes as if the caller were gone. Without that the reply copies its bytes
    // into a buffer belonging to a transaction it has no part in and wakes a thread still
    // linked on another list.
    //
    // A SECOND server, and not a second call to the same one, because the holder of the
    // abandoned cap is already at KICKOS_CAP_REPLY_MAX and can mint no other: there is no
    // staging in which one server holds both the stale cap and the live transaction.
    //
    // The window carries no sleep. Main learns that the second server has taken the
    // aliasing call by RENDEZVOUS on a second endpoint: the token is sent only after that
    // recv returned, and main's recv of it cannot complete earlier. The second endpoint is
    // not a convenience: on one endpoint main's recv could pop a loop call instead of the
    // token, mint a reply cap for it and strand the caller.
    char const SR_GOOD[] = "OK!!";
    char const SR_BAD[] = "BAD!";
    // The abandoned call left call_seq at A and the timeout unwind rolled it to A+1, so the
    // caller's k-th further call runs at A+1+k and the low byte packed in the cap (A) comes
    // round again at k == 255. The loops below are sized so the LAST call is exactly that
    // one: anything shorter and the sequence test refuses the cap before the guard this arm
    // exists for is ever consulted.
    constexpr int SR_SEQ_PERIOD = 1 << 8; // KCAP_REPLY_SEQ_BITS
    constexpr int SR_ALIAS_CALL = SR_SEQ_PERIOD - 1;
    void sr_caller(void*) // caps: done@1, lock@2, E1(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(EP_CALL_SETTLE_NS); // main parks in recv first: the call takes the fastpath
        if (kos_call_timed(3, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US) == -KOS_ETIMEDOUT)
        {
            log_put('T');
        }
        bool ok = true;
        for (int k = 0; k < SR_ALIAS_CALL; k++)
        {
            memcpy(buf, "req2", 4);
            int32_t const rc = kos_call(3, buf, 4, sizeof(buf));
            if (rc != 4 or memcmp(buf, SR_GOOD, 4) != 0)
            {
                ok = false; // one log entry for the whole loop: 255 of these say nothing
            }
        }
        char c = 'X';
        if (ok)
        {
            c = 'K'; // every call, the aliasing one included, got ITS OWN server's bytes
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }
    void sr_second_server(void*) // caps: done@1, E1(FULL)@2, E2(FULL)@3
    {
        char buf[8];
        kos_sleep_ns(EP_CALL_TIMEOUT_WAIT_NS); // outlast the first call's deadline
        for (int k = 0; k < SR_ALIAS_CALL - 1; k++)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            kos_recv(2, buf, sizeof(buf), &info);
            kos_reply(info.reply_cap, SR_GOOD, 4);
        }
        struct kos_recv_info last = {0, KOS_CAP_NONE};
        kos_recv(2, buf, sizeof(buf), &last); // the aliasing call, held unanswered
        kos_send(3, "rdy", 3);                // rendezvous: completes only in main's recv
        // Main runs its reply while we are parked here; the go token releases us.
        kos_recv(3, buf, sizeof(buf), nullptr);
        kos_reply(last.reply_cap, SR_GOOD, 4);
        kos_sem_post(CH_DONE);
    }
    void t_reply_abandoned_cap()
    {
        if (not pool_can_host(2))
        {
            tap::skip("pool too small (2 interdependent workers)");
            return;
        }
        log_reset();
        kos_cap_t ep2 = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_endpoint_create(&ep2) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}, {ep2, CH_FULL}};
        auto cl = kos::thread::spawn_caps(sr_caller, nullptr, "srC", 12, ccaps, 3);
        auto s2 = kos::thread::spawn_caps(sr_second_server, nullptr, "srS", 10, scaps, 3);
        TAP_CHECK(cl.valid() and s2.valid());
        char req[8];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        int32_t const got = kos_recv(g_ep, req, sizeof(req), &info); // the FIRST call
        TAP_CHECK(got == 4 and info.reply_cap != KOS_CAP_NONE);
        // Never replied, and never touched again until the token arrives: the deadline
        // expires under us and this cap is abandoned for the rest of the arm.
        char tok[8];
        int32_t const rdy = kos_recv(ep2, tok, sizeof(tok), nullptr);
        TAP_CHECK(rdy == 3 and memcmp(tok, "rdy", 3) == 0); // the aliasing call is parked
        // Every resolve test but the reply-waiter unlink now passes on this cap.
        TAP_CHECK(kos_reply(info.reply_cap, SR_BAD, 4) == -KOS_ESRCH);
        // Consumed exactly once even on the refusal, so the handle no longer resolves.
        TAP_CHECK(kos_reply(info.reply_cap, SR_BAD, 4) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(info.reply_cap) == -KOS_EBADF);
        TAP_CHECK(kos_send(ep2, "go", 2) == 2); // release the second server's reply
        wait_n(2);
        TAP_CHECK(kos_handle_close(ep2) == 0);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(log_eq("TK")); // timed out, then every call answered by its own server
    }

    // --- Timed recv: the deadline expires with nobody sending ---------------------------
    // Runs in main with NO worker: nothing arrives, so there is no cross-domain copy to
    // make unprivileged.
    void t_recv_timeout()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        char buf[8];
        struct kos_recv_timed_opts opts;
        opts.timeout_us = EP_CALL_TIMEOUT_US;
        opts.info.badge = 0;
        opts.info.reply_cap = KOS_CAP_NONE;
        uint64_t const t0 = kos_clock_now();
        int32_t const n = kos_recv_timed(g_ep, buf, sizeof(buf), &opts);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(n == -KOS_ETIMEDOUT); // expired, and no bytes arrived
        TAP_CHECK(waited_us >= EP_CALL_TIMEOUT_US);
        TAP_CHECK(opts.timeout_us == EP_CALL_TIMEOUT_US); // an input, never written back
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Malformed arguments to the timed forms -----------------------------------------
    // Every refusal here is decided before anything parks or copies, so main runs the whole
    // arm alone. The EFAULT half needs a pointer the caller does not own, which a
    // privileged caller can never present: it lives in t_endpoint_bound with the rest of
    // the bound-check coverage.
    void t_timed_arg_refusals()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        char buf[8];
        // The deadline rides the opts struct, so an opts-less timed recv cannot express
        // one. The KERNEL is the refuser: nothing in the stub may answer this, or the
        // refusal would depend on which libc the caller linked.
        TAP_CHECK(kos_recv_timed(g_ep, buf, sizeof(buf), nullptr) == -KOS_EINVAL);
        // Misalignment is EINVAL and not EFAULT: the kernel reads timeout_us out of this
        // struct with a privileged access, which an unaligned address would fault on parts
        // that trap it.
        alignas(alignof(struct kos_recv_timed_opts))
            unsigned char raw[sizeof(struct kos_recv_timed_opts) + alignof(uint32_t)];
        memset(raw, 0, sizeof(raw));
        struct kos_recv_timed_opts* const skewed =
            reinterpret_cast<struct kos_recv_timed_opts*>(raw + 1);
        TAP_CHECK(kos_recv_timed(g_ep, buf, sizeof(buf), skewed) == -KOS_EINVAL);
        // The timed call packs both lengths into one argument word and SATURATES rather
        // than masking. A masked 512 would arrive as 0 and become a silent zero-length
        // call; the saturated value is still above KOS_EP_MSG_MAX, so the kernel's F4
        // refusal survives the packing. Nothing is sent, so no endpoint state moves.
        TAP_CHECK(kos_call_timed(g_ep, buf, KOS_EP_MSG_MAX + 1, sizeof(buf),
                                 EP_CALL_TIMEOUT_US)
                  == -KOS_EINVAL);
        TAP_CHECK(kos_call_timed(g_ep, buf, 512, sizeof(buf), EP_CALL_TIMEOUT_US)
                  == -KOS_EINVAL);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    uint64_t g_call_unit = 1000000ull;

    // --- Timed call: the expiry unwind REVERTS the D2 boost ------------------------------
    // The only arm that can tell whether endpoint_wait_timeout still calls set_prio on the
    // WAIT_EP_SEND branch: delete that one line and every other timed-call arm stays green,
    // because a boost nobody observes changes no return code.
    //
    // Staging, in units of mtx_time_unit(): the server takes main's plain send first, which
    // seats it as the endpoint's conventional server and is what gives the caller something
    // to boost. It then busy-spins for the rest of the arm, so it is never in recv and the
    // caller must park on send_waiters. The caller (high) wakes mid-spin and calls with a
    // deadline; the spoiler (medium) wakes after that and is held off only by the boost.
    //   'u' before 'm': the boost HELD while the caller was parked.
    //   'm' before 'z': the expiry unwind dropped it, so the spoiler outranks the server
    //                   again. Without the revert the server stays pinned at the caller's
    //                   priority and 'z' comes first.
    void ctr_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        kos_recv(3, buf, sizeof(buf), nullptr); // takes main's plain send; ep->server = us
        log_put('a');
        mtx_spin(g_call_unit * 6);  // the caller wakes and D2-boosts us inside this
        log_put('u');
        mtx_spin(g_call_unit * 8);  // the deadline expires inside this, and reverts us
        log_put('z');
        kos_sem_post(CH_DONE);
    }
    void ctr_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(g_call_unit * 2); // wake mid-spin: the server is not in recv, so we park
        uint32_t const deadline_us = static_cast<uint32_t>((g_call_unit * 8ull) / 1000ull);
        int32_t const rc = kos_call_timed(3, buf, 4, sizeof(buf), deadline_us);
        char c = 'X';
        if (rc == -KOS_ETIMEDOUT)
        {
            c = 'c';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }
    void ctr_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        kos_sleep_ns(g_call_unit * 4); // ready while the server is boosted, so it must wait
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_revert()
    {
        // Ask before spawning: the three workers wait on each other, so a partial set
        // cannot be drained and a guard after the spawns would hang rather than skip.
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::spawn_caps(ctr_server, nullptr, "ctrS", 8, scaps, 3);
        auto cl = kos::thread::spawn_caps(ctr_caller, nullptr, "ctrC", 20, ccaps, 3);
        auto sp = kos::thread::spawn_caps(ctr_spoiler, nullptr, "ctrM", 12, mcaps, 2);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        char warm[4] = {0};
        kos_send(g_ep, warm, 4); // parks root, which is what lets the workers start
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('X') == 0); // the call expired, it did not bounce
        TAP_CHECK(count('a') == 1 and count('u') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST held while the caller was parked
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: the unwind put the server back at base
    }

    // --- Call/reply: info-less receiver bounces a call, D2 boost is REVERTED ------
    // A high caller's slow-path kos_call boosts the low server it targets (D2). An
    // INFO-LESS recv cannot host the call, so recv rejects the caller (-KOS_ENOSYS) and MUST
    // revert that boost. Observables as in the PI donation arm: 'u' before 'm' while
    // boosted, 'm' before 'z' after the revert. Without the revert the server stays pinned
    // above the spoiler and 'z' precedes 'm'.
    //
    // Staged by one semaphore, not by sleeps: the server posts it once the recv has seated it,
    // the caller re-posts for the spoiler just before calling. Every ordering the arm asserts
    // therefore falls out of priority alone.
    Atomic<int32_t, Order::RELAXED> g_ci_rc{-99};
    void ci_server(void*) // caps: done@1, lock@2, E(WAIT)@3, stage@4
    {
        char buf[16];
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#1 (info-less): eats one plain sender; ep->server = us
        log_put('a');
        // Only now is the caller released: the D2 boost is conditional on ep->server, which
        // recv#1 above is what seats, so a caller that ran first would boost nothing.
        kos_sem_post(4);
        mtx_spin(g_call_unit * 4);              // the caller D2-boosted us inside this
        log_put('u');
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#2: reject the parked call (deflate us), eat the 2nd sender
        log_put('z');                          // reached at base prio: spoiler ran first IFF we reverted
        kos_sem_post(CH_DONE);
    }
    void ci_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, stage@4
    {
        char buf[8] = {0};
        kos_sem_wait(4);                       // the server is seated
        kos_sem_post(4);                       // the spoiler is ready from here, and we outrank it
        g_ci_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void ci_spoiler(void*) // caps: done@1, lock@2, stage@3 (medium prio)
    {
        // Index 3, not the 4 its posters use: holding no endpoint cap shifts the same
        // semaphore one slot down. The wrong index returns at once and this logs first,
        // which reads as a lost boost.
        kos_sem_wait(3);
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
        // Ask the pool BEFORE spawning anything: the four workers are mutually
        // dependent, so a partial set cannot be drained. Guarding after the spawns
        // does not skip on a small board, it HANGS.
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (4 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        g_ci_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {g_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_gate, CH_FULL}};
        kos_cap_grant fcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL}};
        auto sv = kos::thread::spawn_caps(ci_server, nullptr, "ciS", 8, scaps, 4);
        auto cl = kos::thread::spawn_caps(ci_caller, nullptr, "ciC", 20, ccaps, 4);
        auto sp = kos::thread::spawn_caps(ci_spoiler, nullptr, "ciM", 12, mcaps, 3);
        // Above the server, so its send is already parked when recv#2 runs. Below it, recv#2
        // BLOCKS for a sender and 'm' precedes 'z' whether or not the reject reverted the
        // boost, leaving the arm reporting ok with half of it disarmed.
        auto fl = kos::thread::spawn_caps(ci_filler, nullptr, "ciF", 10, fcaps, 3);
        // The probe above just held four slots and four stacks, so a failure now is a pool
        // bug, not a small board.
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid() and fl.valid());
        char warm[4] = {0};
        kos_send(g_ep, warm, 4); // recv#1 eats the filler's send, recv#2 eats this one
        wait_n(4);
        TAP_CHECK(kos_handle_close(g_gate) == 0);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const ci_rc = g_ci_rc;
        TAP_CHECK(ci_rc == -KOS_ENOSYS); // the call bounced off the info-less receiver
        TAP_CHECK(count('a') == 1 and count('u') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST held: boosted server outran the spoiler's wake
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: server back at base, spoiler ran before it resumed
    }

    // --- Call/reply: close-instead-of-reply EPIPEs the caller AND yields to it -----
    // A low server takes a high caller's call (D1-boosted to the caller's prio), then closes
    // the reply cap instead of replying. The close arm must (a) wake the caller -KOS_EPIPE
    // and (b) deflate the server BEFORE waking, so the higher caller runs before the server
    // proceeds: 'c' strictly before 's'.
    Atomic<int32_t, Order::RELAXED> g_cc_rc{-99};
    void cc_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_recv(3, buf, sizeof(buf), &info); // info-bearing: hosts the call, D1-boosts us, mints a reply cap
        kos_handle_close(info.reply_cap);     // close instead of reply: EPIPE the caller + deflate us
        log_put('s');                         // server proceeds: must be AFTER the caller ran
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(cc_server, nullptr, "ccS", 8, scaps, 3);
        auto cl = kos::thread::spawn_caps(cc_caller, nullptr, "ccC", 20, ccaps, 3);
        if (not sv.valid() or not cl.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const cc_rc = g_cc_rc;
        TAP_CHECK(cc_rc == -KOS_EPIPE);   // (a) caller woken with EPIPE, not a byte count
        TAP_CHECK(nth('c', 1) < nth('s', 1)); // (b) caller ran before the server proceeded
    }

    // --- Call/reply: happy path: request delivered, reply returned in-place ---
    // A server recvs the request (info-bearing), records it, and replies a known payload;
    // the caller's kos_call returns the reply byte count and the reply OVERWRITES its send
    // buffer (in-place). Both paths are covered: (A) server parked in recv first (fastpath),
    // (B) caller parked in SEND_WAIT first, server recvs later (slowpath).
    Atomic<int32_t, Order::RELAXED> g_echo_reqn{-99}; // request bytes the server observed
    char g_echo_reqbuf[8];           // request content the server observed
    Atomic<int32_t, Order::RELAXED> g_echo_rc{-99};   // caller's kos_call return
    char g_echo_rplbuf[8];           // reply content the caller received in-place
    void echo_server(void*)          // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        int32_t n = kos_recv(2, buf, sizeof(buf), &info);
        g_echo_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_echo_reqbuf)) { k = sizeof(g_echo_reqbuf); }
            memcpy(g_echo_reqbuf, buf, k);
        }
        if (info.reply_cap != KOS_CAP_NONE)
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto sv = kos::thread::spawn_caps(echo_server, nullptr, "echS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath)
            cl = kos::thread::spawn_caps(echo_caller, nullptr, "echC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            // cl is spawned only after sv succeeds, so a skip means either nothing spawned
            // (no sv) or a lone server parked in recv (no cl). Neither can be drained: close
            // and skip. Never fires on the CI targets (>= 2 thread slots).
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        int32_t const echo_rc_a = g_echo_rc;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0); // request delivered
        TAP_CHECK(echo_rc_a == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);  // reply back in-place

        // (B) slowpath: caller parks in SEND_WAIT first, server recvs later.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto cl2 = kos::thread::spawn_caps(echo_caller, nullptr, "echC2", 12, ccaps, 2);
        kos::thread::Handle sv2;
        if (cl2.valid())
        {
            kos_sleep_ns(3000000ull); // let the caller park in SEND_WAIT (slowpath)
            sv2 = kos::thread::spawn_caps(echo_server, nullptr, "echS2", 10, scaps, 2);
        }
        if (not cl2.valid() or not sv2.valid())
        {
            // The caller (spawned first) may be parked in SEND_WAIT: close FIRST so it is
            // EPIPE'd and posts, THEN drain it. Never fires on the CI targets.
            kos_handle_close(g_ep);
            if (cl2.valid()) { wait_n(1); }
            tap::partial("slowpath half not run (pool too small)");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_b = g_echo_reqn;
        int32_t const echo_rc_b = g_echo_rc;
        TAP_CHECK(echo_reqn_b == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(echo_rc_b == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);
    }

    // --- Call/reply: root calls like any other thread --------------------------------
    // The orchestrator IS root, and root holds an ordinary thread-pool slot, so a reply
    // capability can name it. Both dispatch sites are driven against one spawned echo
    // server: (A) the server parked in recv before root calls, (B) root parked in
    // SEND_WAIT first. The server must exist BEFORE the call either way, or the endpoint
    // has no recv holder and the call is -KOS_EPIPE.
    //
    // The server runs at KICKOS_PRIO_MIN, one BELOW root, and both halves rest on that.
    // Above root it can park in recv between the spawn and the call on any tick, which makes
    // half (B) a second fastpath run with the same assertions and nothing to say so; and
    // every donation site is `>`-guarded, so only a server that root outranks makes root DONATE
    // and exercises the revert. The price is that the server is still READY with only its exit
    // left when root resumes, so each half drains it before anything spawns again.
    void t_call_from_root()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}}; // done@1, E(WAIT)@2
        char buf[16];

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        auto sv = kos::thread::spawn_caps(echo_server, nullptr, "rtS", 1, scaps, 2);
        if (not sv.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for 1 thread");
            return;
        }
        kos_sleep_ns(3000000ull); // root yields: the server runs and parks in recv (fastpath)
        memcpy(buf, "ping", 4);
        int32_t rc = kos_call(g_ep, buf, 4, sizeof(buf)); // reply lands back in buf
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);
        kos_sleep_ns(3000000ull); // the server reaches EXITED, so its slot is reclaimable

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        auto sv2 = kos::thread::spawn_caps(echo_server, nullptr, "rtS2", 1, scaps, 2);
        if (not sv2.valid())
        {
            kos_handle_close(g_ep);
            tap::partial("slowpath half not run (pool too small)");
            return;
        }
        memcpy(buf, "ping", 4);
        rc = kos_call(g_ep, buf, 4, sizeof(buf)); // no receiver parked yet: root blocks first
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_b = g_echo_reqn;
        TAP_CHECK(echo_reqn_b == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);
        kos_sleep_ns(3000000ull); // the server reaches EXITED, so its slot is reclaimable
    }

    // --- Call/reply: reply + request truncation (datagram clamp, not an error) ---
    // One call exercises BOTH clamps: the caller sends 8 bytes into a server recv buffer of
    // 3 (request truncated to 3), and the server replies 8 bytes into a caller recv_cap of 3
    // (reply truncated to 3). Neither is an error: the byte counts just clamp.
    Atomic<int32_t, Order::RELAXED> g_trunc_reqn{-99}; // request bytes the server saw (its buffer < send_len)
    char g_trunc_reqbuf[4];
    Atomic<int32_t, Order::RELAXED> g_trunc_rc{-99}; // caller's kos_call return (clamped to recv_cap)
    char g_trunc_rplbuf[4];
    void trunc_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[3]; // smaller than the 8-byte request
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        int32_t n = kos_recv(2, buf, sizeof(buf), &info); // request clamps to 3
        g_trunc_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_trunc_reqbuf)) { k = sizeof(g_trunc_reqbuf); }
            memcpy(g_trunc_reqbuf, buf, k);
        }
        if (info.reply_cap != KOS_CAP_NONE)
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(trunc_server, nullptr, "trS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::spawn_caps(trunc_caller, nullptr, "trC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const trunc_reqn = g_trunc_reqn;
        int32_t const trunc_rc = g_trunc_rc;
        TAP_CHECK(trunc_reqn == 3 and memcmp(g_trunc_reqbuf, "ABC", 3) == 0); // request clamped
        TAP_CHECK(trunc_rc == 3 and memcmp(g_trunc_rplbuf, "123", 3) == 0);   // reply clamped
    }

    // --- Call/reply: a second reply on a consumed cap is rejected -----------------
    // The reply cap is one-shot: the first kos_reply consumes it (empty slot + gen bump), so
    // a second kos_reply on the same handle fails resolve with -KOS_EBADF.
    Atomic<int, Order::RELAXED> g_dr_second{-99}; // second kos_reply rc
    Atomic<int32_t, Order::RELAXED> g_dr_callrc{-99};
    void dr_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_recv(2, buf, sizeof(buf), &info);
        if (info.reply_cap != KOS_CAP_NONE)
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(dr_server, nullptr, "drS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::spawn_caps(dr_caller, nullptr, "drC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const dr_callrc = g_dr_callrc;
        int const dr_second = g_dr_second;
        TAP_CHECK(dr_callrc == 2); // caller got the (single) reply
        TAP_CHECK(dr_second == -KOS_EBADF); // the second reply was rejected
    }

    // --- Call/reply: server dies mid-transaction -> caller EPIPE (teardown arm) ---
    // The server takes the call (REPLY_WAIT, holding the reply cap) then exits WITHOUT
    // replying. cap_teardown walks its table, hits the CAP_REPLY arm, and wakes the parked
    // caller with -KOS_EPIPE.
    Atomic<int32_t, Order::RELAXED> g_sd_callrc{-99};
    void sd_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(sd_server, nullptr, "sdS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath call)
            cl = kos::thread::spawn_caps(sd_caller, nullptr, "sdC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0); // server's WAIT cap already gone -> main's is the last
        int32_t const sd_callrc = g_sd_callrc;
        TAP_CHECK(sd_callrc == -KOS_EPIPE);   // caller woken EPIPE by the reply-cap teardown
    }

    // --- Call/reply: server dies pre-pop -> caller EPIPE (recv_holders -> 0) ------
    // The caller parks in SEND_WAIT (no receiver has popped it yet). MAIN holds the sole
    // WAIT cap; closing it drives recv_holders to 0, which drains send_waiters and EPIPEs
    // the parked call: the pre-pop counterpart to the mid-transaction teardown above.
    Atomic<int32_t, Order::RELAXED> g_pp_callrc{-99};
    void pp_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_pp_callrc = kos_call(2, buf, 4, sizeof(buf)); // parks SEND_WAIT; woken -KOS_EPIPE
        kos_sem_post(CH_DONE);
    }
    void t_call_prepop_death()
    {
        g_pp_callrc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // Caller gets SIGNAL only; MAIN is the sole WAIT holder. No server ever recvs.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::spawn_caps(pp_caller, nullptr, "ppC", 12, ccaps, 2);
        TAP_CHECK(cl.valid()); // spawn failure would hang the drain below
        kos_sleep_ns(3000000ull);               // let the caller park in SEND_WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);  // last WAIT cap -> recv_holders 0 -> EPIPE the call
        wait_n(1);
        int32_t const pp_callrc = g_pp_callrc;
        TAP_CHECK(pp_callrc == -KOS_EPIPE);
    }

    // --- Call/reply: donation ordering (positive) --------------------------------
    // low(8) server, high(20) caller, medium(12) spoiler. On the fastpath call the server is
    // D1-boosted to the caller's prio, so the spoiler (which wakes WHILE the server holds
    // the transaction) cannot preempt: the reply reaches the caller ('c') before the
    // spoiler runs ('m'). The positive counterpart to call_infoless_revert. Without donation
    // the medium spoiler would preempt the low server mid-transaction and 'm' would precede 'c'.
    uint64_t g_don_unit = 1000000ull;
    Atomic<int32_t, Order::RELAXED> g_don_rc{-99};
    char g_don_rpl[8];
    void don_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_recv(3, buf, sizeof(buf), &info); // parks first (no senders); D1-boosted at the call
        log_put('a');
        mtx_spin(g_don_unit * 4); // hold the CPU past the spoiler's wake, at the boosted prio
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::spawn_caps(don_server, nullptr, "dnS", 8, scaps, 3);
        auto cl = kos::thread::spawn_caps(don_caller, nullptr, "dnC", 20, ccaps, 3);
        auto sp = kos::thread::spawn_caps(don_spoiler, nullptr, "dnM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            // Drain whoever spawned (each posts g_done: the caller drives the server through
            // its reply, the spoiler is timed), close the endpoint, skip. Mirrors ci_infoless.
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('c') == 1 and count('m') == 1);
        int32_t const don_rc = g_don_rc;
        TAP_CHECK(don_rc == 5 and memcmp(g_don_rpl, "pong!", 5) == 0); // the transaction completed
        TAP_CHECK(nth('r', 1) < nth('c', 1)); // reply delivered: the caller ran after the server replied
        TAP_CHECK(nth('c', 1) < nth('m', 1)); // DONATION: the reply reached the caller before the spoiler ran
    }

    // --- Call/reply (D3): a donation must survive an UNRELATED recompute ----------
    // t_call_donation covers the boost being APPLIED. These two cover it being KEPT: the
    // server runs an unrelated mutex_unlock mid-transaction, which funnels through
    // thread_effective_prio, and the funnel must re-derive the live donation from the donor
    // structures instead of dropping the server back to base. Two arms because there are
    // two donor kinds and each must hold the boost ALONE:
    //   hold:    a live reply cap (the caller is parked in REPLY_WAIT)
    //   pending: a caller parked in SEND_WAIT on an endpoint this thread serves
    // Same low(8)/high(20)/medium(12) shape as t_call_donation: if the recompute deflates
    // the server to 8, the already-awake spoiler (12) preempts it and 'm' precedes 'r'.
    Atomic<int32_t, Order::RELAXED> g_dh_rc{-99};
    void dh_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        kos_recv(3, buf, sizeof(buf), &info); // parks first (no senders); D1-boosted at the call
        log_put('a');
        mtx_spin(g_don_unit * 4);  // hold the CPU past the spoiler's wake, at the boosted prio
        kos_mutex_unlock(4);      // the recompute under test: the reply donor must hold the boost
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void dh_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call after the server has parked in recv (fastpath)
        memcpy(buf, "req", 3);
        g_dh_rc = kos_call(3, buf, 3, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void dh_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        kos_sleep_ns(g_don_unit * 3); // wake while the server holds the boosted transaction
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_hold()
    {
        log_reset();
        g_don_unit = mtx_time_unit();
        g_dh_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::spawn_caps(dh_server, nullptr, "dhS", 8, scaps, 4);
        auto cl = kos::thread::spawn_caps(dh_caller, nullptr, "dhC", 20, ccaps, 3);
        auto sp = kos::thread::spawn_caps(dh_spoiler, nullptr, "dhM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 5); // the transaction completed
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        // The whole arm: the unlock's recompute did NOT deflate the server.
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same again for the OTHER mint site: the caller parks in SEND_WAIT first and the
    // server's recv pops it, so the reply cap is minted from the server's own syscall
    // rather than from the caller's. Same donor kind as _hold, different site, and the
    // sites link the donor independently.
    void ds_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        mtx_spin(g_don_unit * 3); // awake when the call lands, so the call takes the slowpath
        kos_recv(3, buf, sizeof(buf), &info);
        log_put('a');
        mtx_spin(g_don_unit * 4); // spoiler wakes here
        kos_mutex_unlock(4);     // the recompute under test
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void ds_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 1); // call while the server is spinning, not parked
        memcpy(buf, "req", 3);
        g_dh_rc = kos_call(3, buf, 3, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void ds_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        kos_sleep_ns(g_don_unit * 5); // wake inside the post-recv spin
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_slow()
    {
        log_reset();
        g_don_unit = mtx_time_unit();
        g_dh_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::spawn_caps(ds_server, nullptr, "dsS", 8, scaps, 4);
        auto cl = kos::thread::spawn_caps(ds_caller, nullptr, "dsC", 20, ccaps, 3);
        auto sp = kos::thread::spawn_caps(ds_spoiler, nullptr, "dsM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 5);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same shape, but the donor is a caller parked in SEND_WAIT rather than a reply cap:
    // the server takes and replies to a first call, so no reply cap is live, and the
    // caller's SECOND call arrives while the server is awake (slowpath -> D2 boost).
    void dp_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4
    {
        char buf[16];
        struct kos_recv_info i1 = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        kos_recv(3, buf, sizeof(buf), &i1); // call #1, fastpath
        if (i1.reply_cap != KOS_CAP_NONE)
        {
            char rpl[4];
            memcpy(rpl, "1", 1);
            kos_reply(i1.reply_cap, rpl, 1); // no reply cap live past here
        }
        log_put('a');
        mtx_spin(g_don_unit * 4); // call #2 parks in SEND_WAIT here; the spoiler wakes here
        kos_mutex_unlock(4);     // the recompute under test: the SEND_WAIT donor must hold it
        log_put('r');
        struct kos_recv_info i2 = {0, KOS_CAP_NONE};
        kos_recv(3, buf, sizeof(buf), &i2);
        if (i2.reply_cap != KOS_CAP_NONE)
        {
            char rpl[4];
            memcpy(rpl, "2", 1);
            kos_reply(i2.reply_cap, rpl, 1);
        }
        kos_sem_post(CH_DONE);
    }
    void dp_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call #1 after the server has parked in recv
        memcpy(buf, "a", 1);
        int32_t const r1 = kos_call(3, buf, 1, sizeof(buf));
        memcpy(buf, "b", 1);
        int32_t const r2 = kos_call(3, buf, 1, sizeof(buf)); // server is awake -> slowpath
        g_dh_rc = r1 + r2;
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_pending()
    {
        log_reset();
        g_don_unit = mtx_time_unit();
        g_dh_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::spawn_caps(dp_server, nullptr, "dpS", 8, scaps, 4);
        auto cl = kos::thread::spawn_caps(dp_caller, nullptr, "dpC", 20, ccaps, 3);
        auto sp = kos::thread::spawn_caps(dh_spoiler, nullptr, "dpM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 2); // both transactions completed
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // --- Bus service: per-device slot profiles ---------------------------
    // Gated for FLASH, not for a syscall: the mock backend plus serve_one cost ~1.3 KiB, and
    // the non-selftest bluepill-c8 image has ~1.3 KiB of its 64 KiB left. Every CI gate sets
    // the flag.
#if defined(KICKOS_ENABLE_SELFTEST)
    // A controller has a single live profile register set, so kickos::spi::serve_one keeps one
    // device HANDLE per kos_bus_req.device and re-applies the named one inside every transfer.
    // The class backend under it here is spi_mock.cc, which fills the buffer with the word size
    // of the handle it was given, so a transfer on slot 0 must read back slot 0's word size
    // even after slot 1 was opened; with one global profile the second CONFIG wins and slot 0
    // reads back slot 1's value.
    //
    // MAIN must be the server: a spawned server plus a spawned client is two workers, which a
    // 2-slot pool cannot host, so the client is the one thread spawned.

    // What the client observed, in call order.
    Atomic<int, Order::RELAXED> g_slot_cfg0{-99};
    Atomic<int, Order::RELAXED> g_slot_cfg1{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_rx0{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_rx1{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_unconf{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_oor{-99};
    unsigned char g_slot_b0[2] = {0, 0};
    unsigned char g_slot_b1[2] = {0, 0};

    // Frame + kos_call one XFER of `len` dummy bytes on slot `dev`; copies the reply's
    // rx bytes into `rx`. Returns the service's rx length, or a negative -KOS_E*.
    int32_t slot_xfer(uint8_t dev, size_t len, unsigned char* rx)
    {
        unsigned char buf[32];
        size_t const framing = sizeof(struct kos_bus_req) + sizeof(struct kos_bus_seg);
        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_XFER;
        req->device = dev;
        req->nseg = 1;
        req->region_cap = -1;
        req->offset = 0;
        struct kos_bus_seg* seg =
            reinterpret_cast<struct kos_bus_seg*>(buf + sizeof(struct kos_bus_req));
        seg->len = static_cast<uint16_t>(len);
        seg->flags = 0;
        seg->rsv = 0;
        memset(buf + framing, 0, len);

        int32_t const rc = kos_call(2, buf, framing + len, sizeof(buf));
        if (rc < 0)
        {
            return rc;
        }
        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        if (rsp->status < 0)
        {
            return rsp->status;
        }
        memcpy(rx, buf + sizeof(struct kos_bus_rsp), len);
        return rsp->len;
    }

    // Frame + kos_call one CONFIG on slot `dev` with `word_bits`.
    int slot_config(uint8_t dev, uint8_t word_bits)
    {
        unsigned char buf[32];
        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_CONFIG;
        req->device = dev;
        req->nseg = 0;
        req->region_cap = -1;
        req->offset = 0;
        struct kos_bus_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.hz = 1000000u;
        cfg.word_bits = word_bits;
        cfg.cs_policy = KOS_BUS_CS_NONE;
        memcpy(buf + sizeof(struct kos_bus_req), &cfg, sizeof(cfg));

        int32_t const rc = kos_call(2, buf, sizeof(struct kos_bus_req) + sizeof(cfg), sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        return rsp->status;
    }

    void slot_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_slot_cfg0 = slot_config(0, 8);
        g_slot_cfg1 = slot_config(1, 16);
        g_slot_rx0 = slot_xfer(0, sizeof(g_slot_b0), g_slot_b0);
        g_slot_rx1 = slot_xfer(1, sizeof(g_slot_b1), g_slot_b1);
        unsigned char sink[2];
        g_slot_unconf = slot_xfer(2, sizeof(sink), sink); // in range, never configured
        g_slot_oor = slot_xfer(KOS_BUS_DEV_MAX, sizeof(sink), sink); // out of range
        kos_sem_post(CH_DONE);
    }
    void t_bus_device_slots()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::spawn_caps(slot_client, nullptr, "slot", 12, ccaps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        if (not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }

        struct kos_spi_bus bus;
        struct kos_spi_bus_config bcfg = {0u, KOS_CAP_NONE, KOS_CAP_NONE};
        TAP_CHECK(kos_spi_bus_open(&bus, &bcfg) == 0);
        kickos::spi::SlotTable slots;
        unsigned char msg[64]; // the six requests below are 24 bytes at most
        for (int i = 0; i < 6; i++)
        {
            struct kos_recv_info info = {0u, KOS_CAP_NONE};
            int32_t const n = kos_recv(g_ep, msg, sizeof(msg), &info);
            if (n < 0 or info.reply_cap == KOS_CAP_NONE)
            {
                break;
            }
            kickos::spi::serve_one(&bus, slots, msg, static_cast<size_t>(n), info.reply_cap);
        }
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);

        int const slot_cfg0 = g_slot_cfg0;
        int const slot_cfg1 = g_slot_cfg1;
        TAP_CHECK(slot_cfg0 == 0 and slot_cfg1 == 0);
        int32_t const slot_rx0 = g_slot_rx0;
        int32_t const slot_rx1 = g_slot_rx1;
        int32_t const slot_unconf = g_slot_unconf;
        int32_t const slot_oor = g_slot_oor;
        TAP_CHECK(slot_rx0 == 2 and g_slot_b0[0] == 8 and g_slot_b0[1] == 8); // slot 0 kept its own
        TAP_CHECK(slot_rx1 == 2 and g_slot_b1[0] == 16 and g_slot_b1[1] == 16); // slot 1 too
        TAP_CHECK(slot_unconf == -KOS_EINVAL); // no CONFIG for that slot: refused
        TAP_CHECK(slot_oor == -KOS_EINVAL); // slot >= KOS_BUS_DEV_MAX: refused
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- UART service: the wire ABI over the shared rings -----
    // serve_one touches only the shared block, never a register (the peripheral belongs to
    // the IRQ thread and the domain model enforces that at spawn), so the whole
    // request/reply surface is testable with NO device at all. MAIN is the server, as in
    // t_bus_device_slots, so the client is the one thread spawned.
    //
    // The TX doorbell is structurally NOT coverable here: serve_one rings kos_irq_notify on
    // child cap index 2, which in root's own table is the authority slot, so a
    // root-as-server shape cannot host a line cap there. The real two-thread driver covers
    // it.
    //
    // Keep this at ZERO statics: the client reports back over the SAME endpoint as a final
    // plain send. A 1 KiB static would come out of the tiny boards' user arena and turn a
    // later mem_self_grant probe into a skip.
    struct UartResults
    {
        int wr;        // bytes the ring accepted
        int wr_big;    // a larger write, still inside the 512-byte ring
        int badframe;  // len claiming more than the frame carried
        int block;     // a blocking read
        int rd;        // bytes returned by READ
        int stats_tx;  // tx_bytes the driver counted
        int mode_bad;  // SET_MODE carrying an unknown bit
        int mode_clr;  // SET_MODE clearing the policy
        int mode_set;  // SET_MODE seating KOS_UART_F_NONBLOCK
        unsigned char rdbuf[4];
    };

    // Frame + kos_call one request; returns rsp.status, or rsp.len when status is 0.
    int uart_call(uint8_t op, uint8_t flags, uint16_t len, unsigned char const* payload,
                  unsigned char* out, uint16_t out_max)
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_uart_req req;
        memset(&req, 0, sizeof(req));
        req.op = op;
        req.flags = flags;
        req.len = len;
        memcpy(buf, &req, sizeof(req));
        size_t send_len = sizeof(req);
        if (payload != nullptr)
        {
            memcpy(buf + sizeof(req), payload, len);
            send_len += len;
        }
        int32_t const rc = kos_call(2, buf, send_len, sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        struct kos_uart_rsp rsp;
        memcpy(&rsp, buf, sizeof(rsp));
        if (rsp.status < 0)
        {
            return rsp.status;
        }
        if (out != nullptr and rsp.len <= out_max)
        {
            memcpy(out, buf + sizeof(rsp), rsp.len);
        }
        return static_cast<int>(rsp.len);
    }

    void uart_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        UartResults r;
        memset(&r, 0, sizeof(r));
        unsigned char const tx[4] = {'h', 'i', '!', '\n'};
        r.wr = uart_call(KOS_UART_WRITE, 0, 4, tx, nullptr, 0);
        unsigned char big[200];
        memset(big, 'x', sizeof(big));
        r.wr_big = uart_call(KOS_UART_WRITE, 0, sizeof(big), big, nullptr, 0);
        // len claims more than the frame carried: refused rather than reading past it.
        r.badframe = uart_call(KOS_UART_WRITE, 0, 8, nullptr, nullptr, 0);
        // A blocking read is refused explicitly, never answered with 0 bytes.
        r.block = uart_call(KOS_UART_READ, KOS_UART_F_BLOCK, 4, nullptr, nullptr, 0);
        r.rd = uart_call(KOS_UART_READ, 0, 4, nullptr, r.rdbuf, sizeof(r.rdbuf));
        unsigned char st[sizeof(struct kos_uart_stats)];
        if (uart_call(KOS_UART_STATS, 0, 0, nullptr, st, sizeof(st))
            == static_cast<int>(sizeof(st)))
        {
            struct kos_uart_stats s;
            kickos::console::stats_unpack(&s, st);
            r.stats_tx = static_cast<int>(s.tx_bytes.load(std::memory_order_relaxed));
        }
        // Over the WIRE, not against console::mode_apply directly, so serve_one's dispatch
        // is covered. Accept LAST, so the server can assert the mode was stored.
        r.mode_bad = uart_call(KOS_UART_SET_MODE, 0x80, 0, nullptr, nullptr, 0);
        r.mode_clr = uart_call(KOS_UART_SET_MODE, 0, 0, nullptr, nullptr, 0);
        r.mode_set = uart_call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, nullptr,
                               nullptr, 0);
        // A PLAIN send (no reply cap), which is how the server tells this frame apart
        // from a request.
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }

    void t_uart_service()
    {
#if defined(KICKOS_SELFTEST_NO_UART_SERVICE)
        // Pinned per board (see this app's CMakeLists): this arm's 1 KiB arena block would
        // starve the arena probes. The body below MUST stay compiled so the client entry
        // point keeps a referrer; only the ALLOCATION must not happen.
        tap::skip("pinned: the 1 KiB UART block is reserved for the arena probes here");
        return;
#endif
        // The shared block comes from the arena, not from .bss or the stack: root's stack
        // is 2 KiB on the smallest boards, and static would shrink the arena for
        // every later test.
        void* blk = kos_ram_alloc(sizeof(kickos::uart::Shared));
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare the 1 KiB UART block -- board too small");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, sizeof(kickos::uart::Shared)) == 0);
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(blk);
        kickos::uart::shared_init(sh);
        // Stand in for the IRQ thread: put four bytes in the RX ring so the READ below
        // has something to return. In the real driver only the IRQ thread pushes here.
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        TAP_CHECK(kos_byte_ring_push(&sh->rx, rx, 4) == 4);

        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::spawn_caps(uart_client, nullptr, "uartcl", 12, ccaps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        if (not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        UartResults got;
        memset(&got, 0, sizeof(got));
        unsigned char msg[KOS_EP_MSG_MAX];
        // Bounded so a client dying mid-sequence cannot park root here. MUST cover every
        // request uart_client makes plus its results frame; too low deadlocks wait_n below.
        for (int i = 0; i < 12; i++)
        {
            struct kos_recv_info info = {0u, KOS_CAP_NONE};
            int32_t const n = kos_recv(g_ep, msg, sizeof(msg), &info);
            if (n < 0)
            {
                break;
            }
            if (info.reply_cap == KOS_CAP_NONE)
            {
                if (static_cast<size_t>(n) == sizeof(got))
                {
                    memcpy(&got, msg, sizeof(got));
                }
                break; // the results frame is the client's last word
            }
            // The CONSOLE posture every silicon driver runs. A null here refuses SET_MODE.
            kickos::uart::serve_one(sh, &sh->mode, msg, static_cast<size_t>(n),
                                    info.reply_cap);
        }
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);

        TAP_CHECK(got.wr == 4);     // took every byte offered
        TAP_CHECK(got.wr_big == 200); // still fits the 512-byte ring
        TAP_CHECK(got.badframe == -KOS_EINVAL);
        TAP_CHECK(got.block == -KOS_ENOSYS); // refused, NOT answered with 0 bytes
        TAP_CHECK(got.rd == 4);
        TAP_CHECK(got.rdbuf[0] == 'R' and got.rdbuf[1] == 'X');
        TAP_CHECK(got.rdbuf[2] == 'o' and got.rdbuf[3] == 'k');
        // Counted, not merely present: 4 + 200 accepted bytes.
        TAP_CHECK(got.stats_tx == 204);
        TAP_CHECK(got.mode_bad == -KOS_EINVAL); // unknown bit refused whole
        TAP_CHECK(got.mode_clr == 0);
        TAP_CHECK(got.mode_set == 0);
        // Server-side read: the write serve_one made, not the reply it sent.
        TAP_CHECK(sh->mode == KOS_UART_F_NONBLOCK);
    }
#endif

#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    // --- Bound-check: a recv/send pointer outside the caller's regions -> -1 ------
    // The write-oracle / cross-domain-read is closed the same way as the console
    // buffer: an unprivileged caller cannot launder an un-owned page through IPC.
    Atomic<int32_t, Order::RELAXED> g_ep_badrecv_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_ep_badsend_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_ep_badopts_rc{-99};
    int g_ep_bnd_neg_ran = 0;
    void ep_bound_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        void* bad = kos_guard_addr(); // an arena page granted to no domain
        if (bad != nullptr)
        {
            g_ep_badrecv_rc = kos_recv(2, bad, 8, nullptr); // write oracle -> -KOS_EFAULT
            g_ep_badsend_rc = kos_send(2, static_cast<char const*>(bad), 8); // cross-domain read -> -KOS_EFAULT
            // The opts struct is IN-OUT, and an arena page is granule-aligned, so this
            // clears the alignment gate above and lands on the readable+writable check.
            // The message buffer is a valid stack local: the refusal is about opts alone.
            char obuf[8];
            g_ep_badopts_rc = kos_recv_timed(2, obuf, sizeof(obuf),
                               static_cast<struct kos_recv_timed_opts*>(bad));
            g_ep_bnd_neg_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_bound()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_badrecv_rc = -99; g_ep_badsend_rc = -99;
        g_ep_badopts_rc = -99;
        g_ep_bnd_neg_ran = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}}; // done@1, E@2
        auto w = kos::thread::spawn_caps(ep_bound_worker, nullptr, "epbn", 12, caps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        wait_n(1);
        if (g_ep_bnd_neg_ran)
        {
            int32_t const ep_badrecv_rc = g_ep_badrecv_rc;
            int32_t const ep_badsend_rc = g_ep_badsend_rc;
            int32_t const ep_badopts_rc = g_ep_badopts_rc;
            TAP_CHECK(ep_badrecv_rc == -KOS_EFAULT); // bad recv buffer rejected, never parked
            TAP_CHECK(ep_badsend_rc == -KOS_EFAULT); // bad send buffer rejected, never parked
            TAP_CHECK(ep_badopts_rc == -KOS_EFAULT); // un-owned opts rejected, no deadline read
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
    Atomic<int32_t, Order::RELAXED> g_xd_send_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_xd_recv_rc{-99};
    Atomic<int, Order::RELAXED> g_xd_match{0};
    kos_cap_t g_xd_done = KOS_CAP_NONE; // PRIVATE completion sem: workers post it at CH_DONE, not the shared g_done
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
        int32_t n = kos_recv(2, b, 8, nullptr);
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
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_sem_create(0, &g_xd_done); // PRIVATE: never satisfies another test's wait_n(g_done)
        g_xd_send_rc = -99; g_xd_recv_rc = -99; g_xd_match = 0;
        kos_cap_grant scaps[] = {{g_xd_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}}; // done@1, E(SIGNAL)@2
        kos_cap_grant rcaps[] = {{g_xd_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};   // done@1, E(WAIT)@2
        auto s = kos::thread::spawn_caps(xd_send_worker, sbuf, "xdTx", 12, scaps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false, sbuf, 256);
        auto r = kos::thread::spawn_caps(xd_recv_worker, rbuf, "xdRx", 12, rcaps, 2,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false, rbuf, 256);
        if (not s.valid() or not r.valid())
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
        int32_t const xd_send_rc = g_xd_send_rc;
        int32_t const xd_recv_rc = g_xd_recv_rc;
        TAP_CHECK(xd_send_rc == 8 and xd_recv_rc == 8);
        int const xd_match = g_xd_match;
        TAP_CHECK(xd_match == 1); // the byte-exact payload crossed domains
        TAP_CHECK(kos_handle_close(g_ep) == 0); // both delegated caps already torn down -> freed
        kos_sem_destroy(g_xd_done);
    }

    // One own-create of whichever object type this board still has a pool slot for, reporting
    // which pool answered: an arm that USES the capability it landed on cannot treat a
    // semaphore and a mutex alike. A create allocates its OBJECT before installing the cap, so
    // a pool that empties on the same create that fills the table returns the pool refusal and
    // says nothing about the table; the mutex fallback gets past that. -KOS_EMFILE out of here
    // means the table is full, -KOS_ENOMEM that every pool tried is empty.
    int fill_one_cap_typed(kos_cap_t* out, bool* is_sem)
    {
        *is_sem = true;
        int rc = kos_sem_create(0, out);
        if (rc == -KOS_ENOMEM)
        {
            *is_sem = false;
            rc = kos_mutex_create(out);
        }
        return rc;
    }

    int fill_one_cap(kos_cap_t* out)
    {
        bool is_sem = false;
        return fill_one_cap_typed(out, &is_sem);
    }

    // The low 16 bits of a cap handle are its table slot and the high 16 its cap-gen; the
    // split is fixed fleet-wide (cap.h), so neither is board-derived.
    constexpr kos_cap_t CAP_IDX_MASK = 0xFFFFu;
    constexpr int CAP_GEN_SHIFT = 16;

    // Own-creates until the table refuses. The caller owns every handle written to `held`.
    int fill_table(kos_cap_t* held, bool* held_is_sem, int room)
    {
        int n = 0;
        while (n < room)
        {
            kos_cap_t h = KOS_CAP_NONE;
            bool is_sem = false;
            if (fill_one_cap_typed(&h, &is_sem) != 0)
            {
                break;
            }
            held[n] = h;
            held_is_sem[n] = is_sem;
            n = n + 1;
        }
        return n;
    }

    // --- B3: index 0 is the kernel stdout slot; an own create never lands there ---------
    void t_cap_index0()
    {
        // The reserved plane is never threaded onto the run's free list, so an own
        // sem/endpoint/mutex create cannot pop a well-known slot (0 = console default,
        // 1..FIRST_DYNAMIC-1 = board/service delegation) and lands at
        // >= KOS_CAP_FIRST_DYNAMIC. The checks below fail LOUDLY if the free list is ever
        // built from a lower index. Delegation seats an explicit index and so cannot catch
        // that; only an OWN create can.
        kos_cap_t s = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &s) == 0 and (s & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        kos_cap_t e = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&e) == 0 and (e & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0 and (m & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        TAP_CHECK(kos_handle_close(s) == 0);
        TAP_CHECK(kos_handle_close(e) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);

        // Index 0 is the kernel stdout slot; BOTH of its postures are asserted here.
        // The discriminator is a ZERO-length send: a valid zero-length signal per
        // <kickos/sys.h>, so unlike the 1-byte probe below it puts NO byte on the wire
        // in either posture.
        int32_t const stdout_seated = kos_send(0, "", 0);
        if (stdout_seated == -KOS_EBADF)
        {
            // Pre-publish (g_stdout_target < 0): cap_install_defaults seats NOTHING at
            // index 0, so a send fails cleanly rather than resolving a stale object.
            TAP_CHECK(kos_send(0, "x", 1) == -KOS_EBADF);
        }
        else
        {
            // Post-publish: cap_seat_stdout put a send-only (CAP_SIGNAL) endpoint cap
            // at index 0, so the zero-length signal rendezvoused with the console
            // driver. A rendezvous only completes with a live receiver, so this is the
            // one place the suite proves console output is actually ACKed.
            TAP_CHECK(stdout_seated == 0);
        }

        // Exhaustion: own-creates fill the remaining slots [FIRST_DYNAMIC .. MAX_HANDLES-1]
        // and then fail with -KOS_EMFILE and NOT the -KOS_ENOMEM of an exhausted pool: the
        // reserved range stays off-limits even at the LAST free slot.
        kos_cap_t held[KICKOS_MAX_HANDLES];
        int n = 0;
        while (true)
        {
            kos_cap_t h = KOS_CAP_NONE;
            if (fill_one_cap(&h) != 0)
            {
                break;
            }
            TAP_CHECK((h & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC); // never a reserved slot, not even the last free one
            held[n] = h;
            n = n + 1;
            if (n >= static_cast<int>(sizeof(held) / sizeof(held[0])))
            {
                break;
            }
        }
        TAP_CHECK(n >= 1);
        kos_cap_t full = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        TAP_CHECK(fill_one_cap(&full) == -KOS_EMFILE // the TABLE names itself, not a pool
                  and full == KOS_CAP_NONE);
        full = 0;
        TAP_CHECK(fill_one_cap(&full) == -KOS_EMFILE // idempotent: still refused, no side effect
                  and full == KOS_CAP_NONE);
        for (int i = 0; i < n; i++)
        {
            TAP_CHECK(kos_handle_close(held[i]) == 0);
        }
        kos_cap_t again = KOS_CAP_NONE; // table recovers once slots are freed
        TAP_CHECK(kos_sem_create(0, &again) == 0 and (again & CAP_IDX_MASK) != 0);
        TAP_CHECK(kos_handle_close(again) == 0);
    }

    // --- the SEGMENTED index decode: a live slot at or above the chunk granule ------------
    // Not a detector for a mis-decode: a consistent bijective one relabels slots and this arm
    // still passes. docs/design-capability-table.md section on the chunk boundary has the case.
    void t_cap_chunk_span()
    {
        // cmake/cap_geometry.cmake's target, reaching here as config/cap_width.h's
        // KCAP_CHUNK_TARGET. A table no wider
        // than this compiles the FLAT decode and has no segmented slot to reach, so a hardcoded
        // mirror of the granule would make this arm claim the segmented path on a board that
        // never compiled it.
        constexpr uint32_t CHUNK_SLOTS = KICKOS_CAP_CHUNK_SLOTS;
        constexpr uint32_t TABLE_SLOTS = KICKOS_MAX_HANDLES;

        kos_cap_t held[KICKOS_MAX_HANDLES];
        bool held_is_sem[KICKOS_MAX_HANDLES];
        int const n = fill_table(held, held_is_sem, KICKOS_MAX_HANDLES);
        TAP_CHECK(n >= 1);

        int top = 0;
        bool below_granule = false;
        for (int i = 0; i < n; i++)
        {
            if ((held[i] & CAP_IDX_MASK) > (held[top] & CAP_IDX_MASK))
            {
                top = i;
            }
            if ((held[i] & CAP_IDX_MASK) < CHUNK_SLOTS)
            {
                below_granule = true;
            }
            for (int j = i + 1; j < n; j++)
            {
                // A directory index that decoded to the wrong chunk would still report back
                // the index the install asked for, so only distinctness catches it.
                TAP_CHECK((held[i] & CAP_IDX_MASK) != (held[j] & CAP_IDX_MASK));
            }
        }

        // Gated on the CONFIGURED width, never on the index the fill reached: a wide board
        // whose pools ran dry early must FAIL here, not report PARTIAL.
        if (TABLE_SLOTS <= CHUNK_SLOTS)
        {
            for (int i = 0; i < n; i++)
            {
                TAP_CHECK(kos_handle_close(held[i]) == 0);
            }
            tap::partial("table is %u slot(s): the flat decode, no index reaches the granule",
                         static_cast<unsigned>(TABLE_SLOTS));
            return;
        }
        TAP_CHECK((held[top] & CAP_IDX_MASK) >= CHUNK_SLOTS);
        TAP_CHECK(below_granule); // both sides of the boundary live at once

        // USABLE, not merely numbered: the operation has to reach the object, which is the
        // only proof the directory index and the in-chunk offset recombined onto the entry
        // the install wrote.
        if (held_is_sem[top])
        {
            TAP_CHECK(kos_sem_post(held[top]) == 0);
            TAP_CHECK(kos_sem_wait(held[top]) == 0);
        }
        else
        {
            TAP_CHECK(kos_mutex_lock(held[top]) == 0);
            TAP_CHECK(kos_mutex_unlock(held[top]) == 0);
        }
        TAP_CHECK(kos_handle_close(held[top]) == 0);
        TAP_CHECK(kos_handle_close(held[top]) == -KOS_EBADF); // the entry the close emptied
        for (int i = 0; i < n; i++)
        {
            if (i == top)
            {
                continue;
            }
            // Ordered after the close above on purpose: had the high slot's decode aliased
            // one of these, that close would have emptied this entry too and this would be
            // -KOS_EBADF.
            TAP_CHECK(kos_handle_close(held[i]) == 0);
        }
    }

    // --- the cap-gen half of the handle codec: a recycled slot stales the old handle -------
    void t_cap_gen_reuse()
    {
        kos_cap_t held[KICKOS_MAX_HANDLES];
        bool held_is_sem[KICKOS_MAX_HANDLES];
        int n = fill_table(held, held_is_sem, KICKOS_MAX_HANDLES);
        TAP_CHECK(n >= 1);
        // The table has to be FULL, and -KOS_EMFILE is the only thing that says so. A close
        // then leaves the released slot as the free list's ONLY node, so the next install is
        // forced back onto that same index; with any slot still free the mint lands elsewhere
        // (a release goes to the TAIL, cap.h) and the cap-gen test in cap_lookup stays
        // unreachable behind the empty-slot test ahead of it.
        kos_cap_t refused = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        TAP_CHECK(fill_one_cap(&refused) == -KOS_EMFILE and refused == KOS_CAP_NONE);

        kos_cap_t const stale = held[n - 1];
        n = n - 1;
        TAP_CHECK(kos_handle_close(stale) == 0);
        kos_cap_t fresh = KOS_CAP_NONE;
        bool fresh_is_sem = false;
        TAP_CHECK(fill_one_cap_typed(&fresh, &fresh_is_sem) == 0);
        TAP_CHECK((fresh & CAP_IDX_MASK) == (stale & CAP_IDX_MASK));     // the same slot
        TAP_CHECK((fresh >> CAP_GEN_SHIFT) != (stale >> CAP_GEN_SHIFT)); // a new cap-gen

        // The slot is in range and NOT empty, so the cap-gen comparison is the only test in
        // cap_lookup left that can refuse `stale`. `fresh` on that same index is the control:
        // without it a refusal for any other reason would read identically.
        if (fresh_is_sem)
        {
            TAP_CHECK(kos_sem_post(stale) == -KOS_EBADF);
            TAP_CHECK(kos_sem_post(fresh) == 0);
            TAP_CHECK(kos_sem_wait(fresh) == 0);
        }
        else
        {
            TAP_CHECK(kos_mutex_lock(stale) == -KOS_EBADF);
            TAP_CHECK(kos_mutex_lock(fresh) == 0);
            TAP_CHECK(kos_mutex_unlock(fresh) == 0);
        }
        TAP_CHECK(kos_handle_close(stale) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(fresh) == 0);
        for (int i = 0; i < n; i++)
        {
            TAP_CHECK(kos_handle_close(held[i]) == 0);
        }
    }

    // --- Per-task table width, and the inbound reply bound ------------------------
    //
    // NO new file-scope state below: every worker reports through the shared event log, and
    // a static here would come straight out of the 16 KiB boards' user arena.

    void* units(unsigned n)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(n));
    }
    uint64_t unit_delay(void* arg)
    {
        return g_call_unit * static_cast<uint64_t>(reinterpret_cast<uintptr_t>(arg));
    }

    // Marks one '#' per capability it got: the width it was seated with, minus the reserved
    // plane, minus the one grant landing on a dynamic index (done@1 is below
    // KOS_CAP_FIRST_DYNAMIC and so was never on the free list, lock@2 is above it and was).
    void width_child(void*) // caps: done@1, lock@2
    {
        kos_cap_t held[KICKOS_MAX_HANDLES];
        int n = 0;
        while (n < static_cast<int>(sizeof(held) / sizeof(held[0])))
        {
            kos_cap_t h = KOS_CAP_NONE;
            if (fill_one_cap(&h) != 0)
            {
                break;
            }
            held[n] = h;
            n = n + 1;
        }
        kos_cap_t refused = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        if (fill_one_cap(&refused) == -KOS_EMFILE and refused == KOS_CAP_NONE)
        {
            log_put('E'); // the TABLE refused, not an object pool
        }
        for (int i = 0; i < n; i++)
        {
            if (kos_handle_close(held[i]) == 0)
            {
                log_put('#');
            }
        }
        kos_sem_post(CH_DONE);
    }

    // --- every spawned child gets KICKOS_CAP_CHILD_WIDTH, not root's summed width ---------
    void t_cap_child_width()
    {
        if (not pool_can_host(1))
        {
            tap::skip("pool too small");
            return;
        }
        log_reset();
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // -> done@1, lock@2
        auto w = kos::thread::spawn_caps(width_child, nullptr, "cw", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(count('E') == 1);
        TAP_CHECK(count('#') == KICKOS_CAP_CHILD_WIDTH - KOS_CAP_FIRST_DYNAMIC - 1);
        if (KICKOS_CAP_CHILD_WIDTH == KICKOS_MAX_HANDLES)
        {
            tap::partial("child width %u == root's summed width: the two are one table here",
                         static_cast<unsigned>(KICKOS_CAP_CHILD_WIDTH));
        }
    }

    // Holds A's reply capability across a SECOND recv, so B's call meets the bound. Root's
    // first plain send is what unparks that second recv; the reply to A follows it, and that
    // is what lifts the bound for B's retry. Root's second plain send ends the run.
    void rb_server(void* arg) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        kos_sleep_ns(unit_delay(arg));
        struct kos_recv_info first = {0, KOS_CAP_NONE};
        kos_recv(CH_AUX, b, sizeof(b), &first);
        log_put('1');
        struct kos_recv_info wake = {0, KOS_CAP_NONE};
        kos_recv(CH_AUX, b, sizeof(b), &wake);
        kos_reply(first.reply_cap, "r", 1);
        log_put('2');
        int plains = 0;
        if (wake.reply_cap == KOS_CAP_NONE)
        {
            plains = 1;
        }
        else
        {
            // Only a kernel with no bound puts a call here. Reply to it, and keep serving:
            // the arm must FAIL on the missing refusal, never hang on a stranded caller.
            kos_reply(wake.reply_cap, "r", 1);
        }
        while (plains < 2)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            kos_recv(CH_AUX, b, sizeof(b), &info);
            if (info.reply_cap == KOS_CAP_NONE)
            {
                plains = plains + 1;
                continue;
            }
            kos_reply(info.reply_cap, "r", 1);
        }
        kos_sem_post(CH_DONE);
    }
    void rb_caller_a(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == 1)
        {
            log_put('A');
        }
        kos_sem_post(CH_DONE);
    }
    void rb_caller_b(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == -KOS_EMFILE)
        {
            log_put('E'); // refused against the SERVER's reply bound, not against our table
        }
        kos_sleep_ns(g_call_unit * 12); // past root's first plain send, so A has been replied to
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == 1)
        {
            log_put('K'); // and admitted once A's reply capability was consumed
        }
        kos_sem_post(CH_DONE);
    }

    // `server_delay` decides WHICH probe refuses B. 0 parks the server in recv before either
    // caller runs, so B meets endpoint_call's fastpath probe; a delay past both callers makes
    // B park in CALL_SEND_WAIT first, so the refusal comes back through the recv-side scan.
    void reply_bound_arm(unsigned server_delay, unsigned b_delay)
    {
        // The choreography holds exactly ONE reply capability live and asserts B meets the
        // bound against it. A higher bound needs KICKOS_CAP_REPLY_MAX callers parked before
        // B, which is that many more thread slots and an ordering this log cannot express, so
        // skip rather than assert a bound that is not the configured one.
        if (KICKOS_CAP_REPLY_MAX != 1)
        {
            tap::skip("KICKOS_CAP_REPLY_MAX is %u: this arm only drives a bound of 1",
                      static_cast<unsigned>(KICKOS_CAP_REPLY_MAX));
            return;
        }
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(rb_server, units(server_delay), "rbS", 8, scaps, 3);
        auto ca = kos::thread::spawn_caps(rb_caller_a, units(1), "rbA", 20, ccaps, 3);
        auto cb = kos::thread::spawn_caps(rb_caller_b, units(b_delay), "rbB", 12, ccaps, 3);
        TAP_CHECK(sv.valid() and ca.valid() and cb.valid());
        char plain[4] = {0};
        kos_sleep_ns(g_call_unit * (server_delay + 6));
        kos_send(g_ep, plain, 4); // unpark the second recv, so the server can reply to A
        kos_sleep_ns(g_call_unit * 14);
        kos_send(g_ep, plain, 4); // end the run, whatever the server ended up serving
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('E') == 1);           // B refused while A's reply capability was live
        TAP_CHECK(count('K') == 1);           // and admitted after it was consumed
        TAP_CHECK(count('A') == 1);           // A was never crowded out by B
        TAP_CHECK(nth('E', 1) < nth('2', 1)); // the refusal preceded the reply that lifted it
    }

    // --- the reply bound, met at endpoint_call's fastpath probe ---------------------------
    void t_cap_reply_bound_fast()
    {
        reply_bound_arm(0, 3);
    }

    // --- the same bound, delivered through the recv-side scan of parked callers -----------
    void t_cap_reply_bound_slow()
    {
        reply_bound_arm(4, 0);
    }

    // Serves calls until root's plain send ends the run. arg != 0 consumes the FIRST reply
    // capability with kos_handle_close rather than kos_reply: a release path kos_reply does
    // not cover, and the caller sees -KOS_EPIPE.
    void rp_server(void* arg) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        for (int i = 0; i < 3; i++)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            kos_recv(CH_AUX, b, sizeof(b), &info);
            if (info.reply_cap == KOS_CAP_NONE)
            {
                break; // root's plain send: the run is over, refused callers and all
            }
            if (i == 0 and arg != nullptr)
            {
                kos_handle_close(info.reply_cap);
                log_put('c');
                continue;
            }
            log_put('K');
            kos_reply(info.reply_cap, "r", 1);
        }
        kos_sem_post(CH_DONE);
    }
    void rp_caller(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        int32_t const rc = kos_call(CH_AUX, b, 4, sizeof(b));
        if (rc == -KOS_EPIPE)
        {
            log_put('P');
        }
        if (rc == 1)
        {
            log_put('B');
        }
        kos_sem_post(CH_DONE);
    }

    // --- a reply cap consumed by CLOSE still admits the next caller -----------------------
    void t_cap_reply_release_close()
    {
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::spawn_caps(rp_server, units(1), "rpS", 8, scaps, 3);
        auto ca = kos::thread::spawn_caps(rp_caller, units(1), "rpA", 20, ccaps, 3);
        auto cb = kos::thread::spawn_caps(rp_caller, units(5), "rpB", 12, ccaps, 3);
        TAP_CHECK(sv.valid() and ca.valid() and cb.valid());
        char plain[4] = {0};
        kos_sleep_ns(g_call_unit * 9);
        kos_send(g_ep, plain, 4); // ends the server's run whether or not B got in
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('P') == 1); // A's cap closed, A woken -KOS_EPIPE
        TAP_CHECK(count('K') == 1 and count('B') == 1); // and B admitted behind it
    }

    // Takes a call and EXITS holding the reply capability: the teardown sweep is what has to
    // account it, or the slot's next occupant refuses its own first caller.
    void rr_dying_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_recv(CH_AUX, b, sizeof(b), &info);
        log_put('d');
        kos_sem_post(CH_DONE);
    }

    // --- a slot reclaimed from a server that died mid-call admits its next caller ---------
    void t_cap_reply_slot_reuse()
    {
        if (not pool_can_host(2))
        {
            tap::skip("pool too small (2 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto s1 = kos::thread::spawn_caps(rr_dying_server, nullptr, "rrD", 8, scaps, 3);
        auto c1 = kos::thread::spawn_caps(rp_caller, units(2), "rr1", 12, ccaps, 3);
        TAP_CHECK(s1.valid() and c1.valid());
        wait_n(2);
        TAP_CHECK(count('d') == 1 and count('P') == 1); // died holding a live reply cap

        // Both slots are EXITED now, so these two reclaim them.
        auto s2 = kos::thread::spawn_caps(rp_server, nullptr, "rrS", 8, scaps, 3);
        auto c2 = kos::thread::spawn_caps(rp_caller, units(2), "rr2", 12, ccaps, 3);
        TAP_CHECK(s2.valid() and c2.valid());
        char plain[4] = {0};
        kos_sleep_ns(g_call_unit * 6);
        kos_send(g_ep, plain, 4);
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('K') == 1 and count('B') == 1); // the next occupant admitted its first
    }

    // --- console_publish needs AUTH_CONSOLE; a bad cap is rejected with no side effect --
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console. The rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1); // unprivileged -> -KOS_EPERM
        kos_sem_post(CH_DONE);
    }
    void t_console_publish()
    {
        // From root, which holds AUTH_CONSOLE (this suite declares it): a bad/stale cap
        // is rejected before the deinit/flip, so console ownership stays as the board's
        // service list set it. This test never publishes anything itself: both caps below
        // are invalid, and the child holding no authority is refused by the gate.
        //
        // The -KOS_EBADF assertions are exact for a reason: the AUTH_CONSOLE gate runs
        // before the cap resolve, so a root that lost the bit answers -KOS_EPERM and this
        // test fails rather than passing on the wrong code.
        TAP_CHECK(kos_console_publish(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_console_publish(0x7fffffff) == -KOS_EBADF);
        // Unprivileged child: the privileged-only gate rejects it.
        g_pub_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(pub_denied_worker, nullptr, "pubden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_pub_rc == -KOS_EPERM); // unprivileged console_publish refused
    }

    // --- shutdown is privileged-only: an unprivileged thread cannot end the system ----
    int g_shutdown_rc = -99;
    void shutdown_denied_worker(void*) // caps: done@1
    {
        // Status 0 on purpose: a regression ends the run here with a clean exit
        // status, which the gate sees as a truncated TAP stream.
        g_shutdown_rc = kos_shutdown(0);
        kos_sem_post(CH_DONE);
    }
    void t_shutdown_denied()
    {
        g_shutdown_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(shutdown_denied_worker, nullptr, "sdden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_shutdown_rc == -KOS_EPERM); // unprivileged shutdown refused
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- reboot-to-bootloader is privileged-only: the REFUSAL arm only -------------
    // The privileged arm stays out of this suite deliberately: root calling kos_reboot
    // returns harmlessly on a declining-fallback chip, but on picopi/pizero2350/teensy41 it
    // reboots the board mid-run and truncates the TAP stream. apps/rebootdemo owns it.
    int g_reboot_rc = -99;
    void reboot_denied_worker(void*) // caps: done@1
    {
        g_reboot_rc = kos_reboot();
        kos_sem_post(CH_DONE);
    }
    void t_reboot_denied()
    {
        g_reboot_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(reboot_denied_worker, nullptr, "rbden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_reboot_rc == -KOS_EPERM); // unprivileged reboot refused
    }
#endif // KICKOS_ENABLE_SELFTEST (reboot refusal)

    // --- a syscall buffer that lives in an app GLOBAL, from an unprivileged thread ----
    // On a backend that does not model app static data as an MPU region (every no-MPU
    // chip, and the host sim, whose globals sit outside the mprotect'd arena) the raw
    // writable set collapses to the stack alone; user_writable_ok's static-data
    // fallback is what admits a global buffer.
    //
    // recv validates its buffer BEFORE resolving the cap, so a deliberately invalid
    // cap separates the two answers with no rendezvous and no sender: EBADF means the
    // buffer was admitted, EFAULT means it was not.
    char g_wrbuf[16];
    int32_t g_wrbuf_rc = -99;
    void wrbuf_worker(void*) // caps: done@1
    {
        g_wrbuf_rc = kos_recv(0x7fffffff, g_wrbuf, sizeof(g_wrbuf), nullptr);
        kos_sem_post(CH_DONE);
    }
    void t_writable_global()
    {
        g_wrbuf_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(wrbuf_worker, nullptr, "wrGlob", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_wrbuf_rc == -KOS_EBADF); // not -KOS_EFAULT: the global was writable
    }

    // --- The authority capability: the non-privileged arm of the authority gates ------
    // Each authority gate is `privileged OR holds this AUTH_* bit`; root is privileged,
    // so the rest of the suite only exercises the privileged arm.
    //
    // The child is UNPRIVILEGED and holds AUTH_PINMUX and nothing else, so exactly one
    // gate must accept it and the rest must refuse. Acceptance reads as "not
    // -KOS_EPERM": a gate that lets the call through returns its OWN answer instead
    // (-KOS_ENOSYS on a declining-fallback target like the sim, -KOS_EINVAL where a chip owns
    // the block).
    void auth_noop(void*) {}
    Atomic<int32_t, Order::RELAXED> g_auth_pinmux{-99};    // AUTH_PINMUX held    -> anything but -KOS_EPERM
    Atomic<int32_t, Order::RELAXED> g_auth_shutdown{-99};  // AUTH_SYSTEM absent  -> -KOS_EPERM
    Atomic<int32_t, Order::RELAXED> g_auth_regrant{-99};   // may not hand on a bit it does not hold
    Atomic<int32_t, Order::RELAXED> g_auth_toomany{-99};   // cap_count above the spawn-grant bound
    Atomic<int32_t, Order::RELAXED> g_auth_badbits{-99};   // a bit that is no authority at all
    Atomic<int32_t, Order::RELAXED> g_auth_capsarr{-99};   // the grant ARRAY read, reached past the early refusals
    Atomic<int32_t, Order::RELAXED> g_auth_narrowbad{-99}; // kos_cap_narrow on a cap that is not the authority
    Atomic<int32_t, Order::RELAXED> g_auth_narrowup{-99};  // a mask naming an unheld bit intersects, never grants
    Atomic<int32_t, Order::RELAXED> g_auth_notgained{-99}; // so the gate for that bit still refuses
    Atomic<int32_t, Order::RELAXED> g_auth_narrow{-99};    // giving up the held bit succeeds, needing no authority
    Atomic<int32_t, Order::RELAXED> g_auth_dropped{-99};   // and the gate that accepted it now refuses
    kos_thread_params g_auth_kid;  // deliberately static: see auth_worker
    kos_cap_grant g_auth_two[2];   // ditto
    void auth_worker(void*) // UNPRIVILEGED, authority = AUTH_PINMUX; caps: done@1
    {
        // The bit it HOLDS: past the gate, so pinmux answers for itself.
        g_auth_pinmux = kos_pinmux_set(99u, 0u, 0x10u);
        // A bit it does NOT hold, at a different gate: proves the bits are independent
        // rather than one lump. Safe to call only BECAUSE the child lacks AUTH_SYSTEM;
        // a regression ends the run here with a clean status, which the harness sees
        // as a truncated TAP stream.
        g_auth_shutdown = kos_shutdown(0);
        // Three spawn probes off ONE params struct, all refused before a pool slot is
        // claimed, so their codes are deterministic even on a full pool. One struct
        // because a frame-local kos_thread_params costs an inline zero-init per site.
        //
        // g_auth_kid and g_auth_two are globals on purpose: thread_spawn reads the params
        // struct and the grant array through user_readable_ok, so a caller may keep either
        // in static data, and that is what this covers.
        kos_thread_params& kid = g_auth_kid;
        kos_thread_t kidh = KOS_THREAD_NONE;
        kid.entry = auth_noop;
        kid.prio = 9;
        // Narrow-only, the same rule a cap_grant mask obeys: holding AUTH_PINMUX does not
        // let it seat AUTH_SYSTEM on a child.
        kid.authority = KOS_AUTH_SYSTEM;
        g_auth_regrant = kos_thread_spawn(&kid, &kidh);
        // cap_count is bounded by KICKOS_MAX_SPAWN_GRANTS, which is the spawn stager's
        // caller-stack budget and NOT the child table's ceiling. 255 exceeds it on every
        // board. Refused on the COUNT, before the array is read: g_auth_two is two
        // entries long, so a bound checked after the read would fault here.
        g_auth_two[0] = {CH_DONE, CH_FULL};
        g_auth_two[1] = {CH_DONE, CH_FULL};
        kid.caps = g_auth_two;
        kid.cap_count = 255;
        kid.authority = KOS_AUTH_PINMUX;
        g_auth_toomany = kos_thread_spawn(&kid, &kidh);
        // A bit no gate reads is refused, not masked off. It has to come from ABOVE the
        // six defined authorities: the authority word has its own numbering, separate
        // from the shared rights byte, so bits 0..5 are all real authorities and an
        // object right like KOS_CAP_WAIT is not a distinguishable wrong value here.
        kid.cap_count = 1;
        kid.authority = 1u << 6;
        g_auth_badbits = kos_thread_spawn(&kid, &kidh);
        // The three probes above are refused before the delegation loop, so none of them
        // reads g_auth_two. Covering the static grant ARRAY needs a probe that gets that
        // far: an unresolvable source_cap is refused -KOS_EBADF from inside the loop,
        // reachable only once the array is admitted. Refused before a slot is claimed.
        g_auth_two[0] = {0x7fffffff, CH_FULL};
        kid.authority = 0;
        g_auth_capsarr = kos_thread_spawn(&kid, &kidh);
        // Stage 4, and the only in-env witness of it: giving up an authority. Narrowing
        // an object cap is refused, so the sem cap at CH_DONE is the negative arm, and
        // it must run BEFORE the drop, since it is the last thing here that still needs
        // a live authority cap to be meaningful.
        g_auth_narrowbad = kos_cap_narrow(CH_DONE, 0);
        // A NONZERO mask naming a bit this worker does not hold. The mask is not the new
        // word: narrowing intersects, so asking for AUTH_SYSTEM here must not grant it.
        // A verbatim-seat bug (word = mask) passes a mask-0 test and fails this one.
        g_auth_narrowup = kos_cap_narrow(KOS_CAP_AUTHORITY, KOS_AUTH_PINMUX | KOS_AUTH_SYSTEM);
        g_auth_notgained = kos_shutdown(0); // still refused: AUTH_SYSTEM was never held
        // Needs no authority of its own, and cannot widen: mask 0 gives up everything.
        g_auth_narrow = kos_cap_narrow(KOS_CAP_AUTHORITY, 0);
        // The SAME gate that answered for itself at the top of this worker now refuses.
        g_auth_dropped = kos_pinmux_set(99u, 0u, 0x10u);
        kos_sem_post(CH_DONE);
    }
    void t_authority_cap()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(auth_worker, nullptr, "authW", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, /*authority=*/KOS_AUTH_PINMUX);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // Grouped: each TAP_CHECK carries __FILE__ and its stringified condition as
        // rodata.
        int32_t const auth_pinmux = g_auth_pinmux;
        TAP_CHECK(auth_pinmux != -KOS_EPERM and auth_pinmux < 0);
        int32_t const auth_shutdown = g_auth_shutdown;
        TAP_CHECK(auth_shutdown == -KOS_EPERM);
        int32_t const auth_regrant = g_auth_regrant;
        int32_t const auth_toomany = g_auth_toomany;
        int32_t const auth_badbits = g_auth_badbits;
        int32_t const auth_capsarr = g_auth_capsarr;
        TAP_CHECK(auth_regrant == -KOS_EPERM and auth_toomany == -KOS_EINVAL
                  and auth_badbits == -KOS_EINVAL and auth_capsarr == -KOS_EBADF);
        int32_t const auth_narrowbad = g_auth_narrowbad;
        int32_t const auth_narrow = g_auth_narrow;
        int32_t const auth_dropped = g_auth_dropped;
        TAP_CHECK(auth_narrowbad == -KOS_EINVAL and auth_narrow == 0
                  and auth_dropped == -KOS_EPERM);
        int32_t const auth_narrowup = g_auth_narrowup;
        int32_t const auth_notgained = g_auth_notgained;
        TAP_CHECK(auth_narrowup == 0 and auth_notgained == -KOS_EPERM);
    }

    // --- Peripheral enable: possession is the whole gate ------------------------
    // kos_periph_enable is authorised by holding a live ARCH_MPU_DEV region whose base
    // is EXACTLY the argument. No authority bit gates it. The possession check runs
    // before the chip backend, so both arms below stop in kernel code on every target
    // and never reach silicon.
    //
    // Runs in an UNPRIVILEGED child in every posture: a privileged caller bypasses
    // possession, so from root the gate is unreachable.
    //
    // Arm 1 holds no DEV region at all. Arm 2 holds an R|W region whose base matches
    // the argument exactly and must STILL be refused, which is what pins the
    // ARCH_MPU_DEV attribute filter rather than the base match alone.
    //
    // Both arms discriminate: with the possession check removed the sim answers
    // -KOS_ENOSYS (the arch_periph_enable fallback), and a chip with a table answers 0 or
    // -KOS_EINVAL. None of those is -KOS_EPERM.
    //
    // The complementary PASS arm (a holder reaching the backend) belongs to an enforcing
    // board: the one DEV window the sim mints is at its fake register block, and
    // arch_periph_enable has no sim backend to reach.
    constexpr uintptr_t PE_BASE = 0x40000000u; // peripheral space, never a code/data/stack base
    Atomic<int32_t, Order::RELAXED> g_pe_unheld{1}; // sentinel: the contract returns 0 or a negative code
    Atomic<int32_t, Order::RELAXED> g_pe_ram{1};
    Atomic<int, Order::RELAXED> g_pe_ram_ran{0};
    void periph_enable_worker(void*) // caps: g_done@1 (CH_DONE)
    {
        g_pe_unheld = kos_periph_enable(PE_BASE);
        // Smallest region this backend can describe, so the arena spend is one block
        // (kos_ram_alloc is a bump allocator with no free).
        void* p = kos_ram_alloc(1);
        if (p != nullptr and kos_mem_self_grant(p, 1) == 0)
        {
            g_pe_ram = kos_periph_enable(reinterpret_cast<uintptr_t>(p));
            g_pe_ram_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_periph_enable_unheld()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(periph_enable_worker, nullptr, "peW", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        int32_t const pe_unheld = g_pe_unheld;
        TAP_CHECK(pe_unheld == -KOS_EPERM);
        // Arm 2 needs one arch_ram_region_size(1) block, so it runs only on a board whose
        // arena still has one past kmain's two boot stacks and the default stack the
        // thread pool bump-allocates per concurrently-live slot. That pool, not a test
        // registered later, is the dominant arena consumer by this point.
        int const pe_ram_ran = g_pe_ram_ran;
        TAP_CHECK(pe_ram_ran == 1);
        int32_t const pe_ram = g_pe_ram;
        TAP_CHECK(pe_ram == -KOS_EPERM);
    }

    // --- Privileged register write: the same possession gate, plus the refusal ------
    // Arm 2 finds its DEV window by TRYING the spawn, not by kos_grant_probe, which
    // needs KICKOS_ENABLE_SELFTEST and would drop the arm on a production-ABI build. A
    // board that mints no window reports PARTIAL.
    // Arm 3 runs from the UNHELD worker: alignment and wrap are checked before
    // possession, so only an unheld caller separates -KOS_EINVAL from the -KOS_EPERM a
    // dropped check would give.
    // Arm 4 reuses arm 2's window; a second DEV grant would cost another arena block and
    // t_dev_window_exclusive would refuse it.
    // PRW_OFFSET must be 4-ALIGNED or arm 2 stops short of the chip layer. The BASE is
    // what keeps it unnameable: the discovery loop steps 0x1000 and the only allowlist in
    // the tree is XMC4800's at 0x40030200, so no candidate base matches an entry.
    constexpr uintptr_t PRW_OFFSET = 0x4u;
    // Pow2 >= the 32 B PMSA minimum; the discovery step is a multiple of it, so every
    // candidate base stays WIN-aligned (PMSA masks an unaligned base down).
    constexpr uint32_t PRW_WIN = 0x100u;
    // ONE byte of .bss carries every arm's verdict, not a result word per arm. Sizing
    // matters here: bluepill-c8's boot arena has ZERO slack (2560 B holds a 512 B idle
    // plus a 2048 B root stack exactly), so any app static RAM this file adds fails the
    // boot-arena link ASSERT, and microbit's 16 KiB arena starves mem_self_grant. The
    // two workers write it in sequence (each is joined on CH_DONE before the next
    // spawns), so the load/store pair below needs no atomicity.
    constexpr unsigned PRW_UNHELD_OK = 1u << 0;     // arm 1: unheld base refused
    constexpr unsigned PRW_HELD_RAN = 1u << 1;      // arm 2: a window was minted
    constexpr unsigned PRW_HELD_OK = 1u << 2;       // arm 2: declined by the chip layer
    constexpr unsigned PRW_MISALIGNED_OK = 1u << 3; // arm 3: offset not 4-aligned
    constexpr unsigned PRW_WRAP_OK = 1u << 4;       // arm 3: base + offset wraps
    constexpr unsigned PRW_PAST_END_OK = 1u << 5;   // arm 4: first word beyond the window
    constexpr unsigned PRW_FAR_OK = 1u << 6;        // arm 4: 4 KiB beyond the window
    Atomic<unsigned char, Order::RELAXED> g_prw{0};
    void periph_reg_write_held_worker(void* arg) // caps: g_done@1 (CH_DONE)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        unsigned seen = PRW_HELD_RAN;
        int32_t const held = kos_periph_reg_write(win, PRW_OFFSET, 0);
        if (held == -KOS_ENOSYS or held == -KOS_EINVAL)
        {
            seen |= PRW_HELD_OK;
        }
        if (kos_periph_reg_write(win, PRW_WIN, 0) == -KOS_EPERM)
        {
            seen |= PRW_PAST_END_OK;
        }
        if (kos_periph_reg_write(win, 0x1000u, 0) == -KOS_EPERM)
        {
            seen |= PRW_FAR_OK;
        }
        g_prw = static_cast<unsigned char>(g_prw | seen);
        kos_sem_post(CH_DONE);
    }
    void periph_reg_write_worker(void*) // caps: g_done@1 (CH_DONE)
    {
        unsigned seen = 0;
        if (kos_periph_reg_write(PE_BASE, PRW_OFFSET, 0) == -KOS_EPERM)
        {
            seen |= PRW_UNHELD_OK;
        }
        // Both malformed-request refusals are checked from an UNHELD caller, which is
        // what makes them discriminate: they run AHEAD of possession, so dropping either
        // check falls through to the possession walk and answers -KOS_EPERM. From a
        // holder the code is -KOS_EINVAL either way (an untabled offset earns the same
        // code), so the arm would be vacuous there.
        if (kos_periph_reg_write(PE_BASE, 0x2u, 0) == -KOS_EINVAL)
        {
            seen |= PRW_MISALIGNED_OK;
        }
        // PE_BASE + ~0x3 wraps at every uintptr_t width (the offset exceeds
        // UINTPTR_MAX - PE_BASE on 32-bit and 64-bit alike), so -KOS_EINVAL is exact
        // here and not width-dependent. The offset is 4-aligned, so the check above
        // cannot answer for this one.
        if (kos_periph_reg_write(PE_BASE, ~static_cast<uintptr_t>(0x3u), 0) == -KOS_EINVAL)
        {
            seen |= PRW_WRAP_OK;
        }
        g_prw = static_cast<unsigned char>(g_prw | seen);
        kos_sem_post(CH_DONE);
    }
    void t_periph_reg_write_unheld()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(periph_reg_write_worker, nullptr, "prwW", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        unsigned char const prw = g_prw;
        TAP_CHECK((prw & PRW_UNHELD_OK) != 0);
        // Arm 3: no DEV window needed, so these run on EVERY board including the ones
        // that mint none.
        TAP_CHECK((prw & PRW_MISALIGNED_OK) != 0);
        TAP_CHECK((prw & PRW_WRAP_OK) != 0);

        // Arms 2 and 4 share one window.
        kos::thread::Handle holder;
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 0x1000u)
        {
            holder = kos::thread::spawn(periph_reg_write_held_worker,
                                        reinterpret_cast<void*>(b), "prwH", 10,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                        nullptr, 0, nullptr, 0,
                                        reinterpret_cast<void*>(b), PRW_WIN,
                                        caps, 1, KOS_AUTH_MEMORY);
            if (holder.valid())
            {
                break;
            }
            if (holder.error() == -KOS_ENOMEM)
            {
                break; // thread pool, not the window: no later base can succeed either
            }
        }
        if (not holder.valid())
        {
            // The sim admits exactly ONE DEV window shape (64 KiB, at the base its fake
            // register block landed on), and PRW_WIN is not it, so this discovery loop
            // finds nothing there. The held arms it drops are covered on the host by
            // periph_reg_write_mask below, which holds that window.
            tap::partial("board mints no free DEV window, so the held arm runs on enforcing "
                         "boards (e.g. the qemu base variant) and, on the host, in "
                         "periph_reg_write_mask");
            return;
        }
        wait_n(1);
        unsigned char const prw_held = g_prw;
        TAP_CHECK((prw_held & PRW_HELD_RAN) != 0);
        TAP_CHECK((prw_held & PRW_HELD_OK) != 0);
        // Arm 4. -KOS_ENOSYS on either of these would mean the offset reached the chip
        // layer, so they discriminate on a board with a backend AND on one without.
        TAP_CHECK((prw_held & PRW_PAST_END_OK) != 0);
        TAP_CHECK((prw_held & PRW_FAR_OK) != 0);
    }

    // --- The allowlist and its VALUE MASK, reached on the host ---------------------
#if KICKOS_ARCH_SIM
    // xmc4800 is the only chip with a real backend, so on every other target the seam
    // answers -KOS_ENOSYS and NO ctest anywhere reaches the allowlist match or the mask
    // compare. arch/sim/sim.cc models a write-PV-only register block over real host
    // pages so this arm reaches both. The path is the SHARED one: the same
    // KOS_SYS_PERIPH_REG_WRITE dispatch arm and the same caller_holds_mmio_reg. Only the
    // table and the store target are sim-side.
    //
    // What the sim CANNOT model is the bus classification itself: nothing here stops the
    // worker storing to its own window directly, so the test seeds and reads the
    // register that way. Under test is the KERNEL's narrowing of the seam, and the
    // read-back is the only evidence of what the seam did or did not store.
    //
    // The sim maps the block at the FIRST of these candidates the host leaves free and
    // publishes that base; one fixed address would make this arm's premise a bet on the
    // process address space. This list and its ORDER must equal arch/sim/sim.cc's
    // SIM_PVREG_BASES, and PVS_WIN / PVS_REG / PVS_BEYOND must equal its
    // SIM_PVREG_WINDOW and its first two allowlist entries. A drift shows up as every
    // candidate being refused, never as a pass.
    constexpr uintptr_t PVS_BASES[] = {
        0x40000000u, 0x100000000ull, 0x400000000ull, 0x10000000000ull, 0x100000000000ull,
    };
    constexpr uint32_t PVS_WIN = 0x10000u;       // the only DEV window shape the sim admits
    constexpr uintptr_t PVS_REG = 0x010u;        // the masked entry, inside the window
    constexpr uintptr_t PVS_UNLISTED = 0x014u;   // inside the window, not on the table
    constexpr uintptr_t PVS_BEYOND = PVS_WIN;    // on the table, OUTSIDE the window
    constexpr uint32_t PVS_MASK = 0x0000C3FFu;   // the entry's whole grant
    constexpr uint32_t PVS_IN = 0x000080FFu;     // a strict subset of it
    // Bit 16 is outside the mask, and the in-mask bits differ from PVS_IN, so a silent
    // TRIM of this value is distinguishable from the refusal by read-back alone.
    constexpr uint32_t PVS_OFF = 0x00010042u;
    constexpr unsigned PVS_RAN = 1u << 0;
    constexpr unsigned PVS_STORE_OK = 1u << 1;      // in-mask value accepted
    constexpr unsigned PVS_STORE_LANDED = 1u << 2;  // and read back exact
    constexpr unsigned PVS_FULLMASK_OK = 1u << 3;   // the whole mask is inside the grant
    constexpr unsigned PVS_OFF_REFUSED = 1u << 4;   // off-mask value -KOS_EINVAL
    constexpr unsigned PVS_OFF_NOTRIM = 1u << 5;    // and the register kept its value
    constexpr unsigned PVS_UNLISTED_OK = 1u << 6;   // untabled offset refused, no store
    constexpr unsigned PVS_BEYOND_OK = 1u << 7;     // tabled but uncontained: -KOS_EPERM
    // Sim-only, so this byte costs no static RAM on a board with an arena floor.
    Atomic<unsigned char, Order::RELAXED> g_pvs{0};
    void pvs_worker(void* arg) // caps: g_done@1 (CH_DONE)
    {
        uintptr_t const PVS_BASE = reinterpret_cast<uintptr_t>(arg);
        unsigned seen = PVS_RAN;
        volatile uint32_t* const reg =
            reinterpret_cast<volatile uint32_t*>(PVS_BASE + PVS_REG);
        volatile uint32_t* const unlisted =
            reinterpret_cast<volatile uint32_t*>(PVS_BASE + PVS_UNLISTED);
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_IN) == 0)
        {
            seen |= PVS_STORE_OK;
        }
        if (*reg == PVS_IN)
        {
            seen |= PVS_STORE_LANDED;
        }
        // The mask's own value must be admitted: this pins the refusal at the mask EDGE,
        // so a predicate that refuses more than (value & ~mask) cannot pass.
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_MASK) == 0 and *reg == PVS_MASK)
        {
            seen |= PVS_FULLMASK_OK;
        }
        kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_IN); // known word before the refusal
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_OFF) == -KOS_EINVAL)
        {
            seen |= PVS_OFF_REFUSED;
        }
        if (*reg == PVS_IN)
        {
            seen |= PVS_OFF_NOTRIM;
        }
        *unlisted = 0;
        if (kos_periph_reg_write(PVS_BASE, PVS_UNLISTED, 0x1u) == -KOS_EINVAL
            and *unlisted == 0)
        {
            seen |= PVS_UNLISTED_OK;
        }
        // Named by the table, one word past the held window: the kernel's containment
        // check is the only thing that refuses it, and -KOS_EPERM (not -KOS_EINVAL)
        // is what proves the refusal came from there and not from the chip layer.
        if (kos_periph_reg_write(PVS_BASE, PVS_BEYOND, 0x1u) == -KOS_EPERM)
        {
            seen |= PVS_BEYOND_OK;
        }
        g_pvs = static_cast<unsigned char>(g_pvs | seen);
        kos_sem_post(CH_DONE);
    }
    void t_periph_reg_write_mask()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        // Try every candidate, in the sim's own order: exactly one is mapped, and which
        // one depends on the host's address space, not on this test.
        kos::thread::Handle w;
        for (uintptr_t b : PVS_BASES)
        {
            w = kos::thread::spawn(pvs_worker, reinterpret_cast<void*>(b), "pvsW", 10,
                                   KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                   nullptr, 0, reinterpret_cast<void*>(b), PVS_WIN,
                                   caps, 1, KOS_AUTH_MEMORY);
            if (w.valid())
            {
                break;
            }
            if (w.error() == -KOS_ENOMEM)
            {
                break; // thread pool, not the window: no later candidate can succeed
            }
        }
        if (not w.valid())
        {
            // No skip: the sim maps the block at one of PVS_BASES unless the host owns
            // ALL of them, which is not a layout this test can be run in. So a refusal
            // is a regression in the grant path, in the sim's mapping loop, or a drift
            // between the two lists. A skip here would also fail the sim gate
            // (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            tap::fail("no candidate DEV window at the sim's fake register block "
                      "(last rc %d) -- PVS_BASES drifted from SIM_PVREG_BASES, or the "
                      "host owns every candidate", w.error());
            return;
        }
        wait_n(1);
        unsigned char const pvs = g_pvs;
        TAP_CHECK((pvs & PVS_RAN) != 0);
        TAP_CHECK((pvs & PVS_STORE_OK) != 0);
        TAP_CHECK((pvs & PVS_STORE_LANDED) != 0);
        TAP_CHECK((pvs & PVS_FULLMASK_OK) != 0);
        TAP_CHECK((pvs & PVS_OFF_REFUSED) != 0);
        TAP_CHECK((pvs & PVS_OFF_NOTRIM) != 0);
        TAP_CHECK((pvs & PVS_UNLISTED_OK) != 0);
        TAP_CHECK((pvs & PVS_BEYOND_OK) != 0);
    }
#endif // KICKOS_ARCH_SIM

    // --- No privilege minting after boot ---------------------------------------
    // syscall_thread.cc refuses a privileged child to an unprivileged caller. That
    // refusal is the whole of "only idle is privileged once boot is over", so it is
    // asserted directly rather than inferred from a posture.
    //
    // Called from ROOT, which is unprivileged, so the refusal costs no thread slot and
    // no arena block (the 16 KiB boards have neither to spare). A posture in which root
    // were privileged turns this red rather than vacuous: the spawn would succeed.
    void escalate_noop(void*) {}
    void t_privileged_spawn_refused()
    {
        TAP_CHECK(kos::thread::spawn(escalate_noop, nullptr, "escd", 10, KOS_POLICY_FIFO,
                                     0, /*privileged=*/true).error()
                  == -KOS_EPERM);
    }

    // --- Join: the death is observed, and an unreclaimed exit still resolves ----
    // Root's slot is the FIRST allocation the thread pool ever makes, so it is index 0 at
    // generation 0 on every board and posture, and handle_for(0) is the bare 0. Change
    // that encoding and the two cases below silently name some other thread.
    constexpr kos_thread_t ROOT_THREAD = 0;
    // Generous, and only ever spent by a refusal: every join it bounds must answer without
    // parking, so an expiry here is the failure and not the schedule.
    constexpr uint32_t JOIN_GENEROUS_US = 60000;
    // The target holds itself alive across the join below, which is the only thing that
    // separates the PARKED path from the already-EXITED early return: both answer 0, and
    // an instant answer is the signature of the wrong one.
    constexpr uint32_t JOIN_PARK_US = 20000;
    constexpr uint64_t JOIN_PARK_NS = 20000000ull;

    // caps: none. Outlives the join that waits for it, then exits.
    void join_target(void*)
    {
        kos_sleep_ns(JOIN_PARK_NS);
    }

    // caps: none. Exists only to occupy a slot and free it again.
    void join_probe(void*) {}

    void join_stranger(void*) // caps: done@1, lock@2
    {
        // Root leaves spawner_tag at KILL_TAG_NONE and kill_tag_of never answers NONE, so
        // root is nobody's child. Issued from a CHILD because root naming itself is the
        // -KOS_EDEADLK case below and would witness nothing about the gate.
        char c = 'x';
        if (kos_thread_join(ROOT_THREAD, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_thread_join()
    {
        log_reset();
        // t0 is read BEFORE the spawn. The target outranks root and thread_spawn runs
        // IRQ-masked, so an interrupt landing in that syscall reschedules to the target the
        // instant the mask drops: it reaches its sleep first, and a t0 taken afterwards
        // excludes that head start from an interval the check below requires to CONTAIN the
        // sleep.
        uint64_t t0 = kos_clock_now();
        // One slot at a time: the target is joined before the stranger is spawned.
        auto w = kos::thread::spawn(join_target, nullptr, "join", 10);
        TAP_CHECK(w.valid());
        // PARKED path, and the elapsed time is what says so: the target sleeps
        // JOIN_PARK_NS before it exits, so a join that answers 0 in less than that
        // answered from the EXITED early return instead, and the next case would be the
        // only one this arm ever ran.
        int const parked_rc = w.join();
        uint32_t waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(parked_rc == 0);
        TAP_CHECK(waited_us >= JOIN_PARK_US); // the target's own exit is what woke it
        // The generation bumps at RECLAIM and not at exit, and root has spawned nothing
        // since, so this handle still names the slot its EXITED occupant holds. The bound
        // is what makes the case total instead of a hang: a kernel that read that state as
        // "still running" answers -KOS_ETIMEDOUT here, and one that read it as a stale
        // handle answers -KOS_EBADF.
        t0 = kos_clock_now();
        int const exited_rc = kos_thread_join(w.id(), JOIN_GENEROUS_US);
        waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(exited_rc == 0);
        TAP_CHECK(waited_us < JOIN_PARK_US); // answered from the slot, with nothing to wait for
        // Refusals that need no target thread: a handle the pool never seats, one whose
        // index is past every slot, and root naming itself.
        TAP_CHECK(kos_thread_join(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(ROOT_THREAD, JOIN_GENEROUS_US) == -KOS_EDEADLK);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // done@1, lock@2
        auto s = kos::thread::spawn_caps(join_stranger, nullptr, "jstr", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // parenthood is the whole gate, and nothing delegates it
    }

    // --- Join: a handle whose slot changed hands --------------------------------
    // thread_resolve has two ways to answer nullptr, and the refusals in t_thread_join
    // reach only the first: KOS_THREAD_NONE and 0x7fffffff both mask to an index no pool
    // ever seats, so they leave through the range check and the generation compare below
    // it is never executed.
    //
    // The reach is bought by RECLAIM. The pool hands out the LOWEST exited slot, and the
    // first target took that slot when it was spawned, so every slot below it was live and
    // still is: the second spawn therefore lands on the first target's slot and bumps its
    // generation. The first handle then carries an index the pool does seat and a
    // generation nothing holds, which is the second branch and nothing else.
    void t_join_stale_gen()
    {
        auto first = kos::thread::spawn(join_probe, nullptr, "jgn1", 10);
        TAP_CHECK(first.valid());
        TAP_CHECK(first.join() == 0); // EXITED, and its slot is now the lowest reclaimable
        kos_thread_t const stale = first.id();
        auto second = kos::thread::spawn(join_probe, nullptr, "jgn2", 10);
        if (not second.valid())
        {
            tap::skip("pool too small");
            return;
        }
        // Same slot, new generation: the two handles differ, and the second one resolves.
        TAP_CHECK(second.id() != stale);
        TAP_CHECK(kos_thread_join(stale, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(second.join() == 0);
    }

    // --- Join: a target that outlives its deadline ------------------------------
    constexpr uint32_t JOIN_TIMEOUT_US = 4000;
    // 20x the deadline, not 1x: at 1x the worker could reach its own exit first and the
    // join would legitimately answer 0.
    constexpr uint64_t JOIN_OUTLIVE_NS = 80000000ull;

    void join_slow(void*) // caps: none
    {
        kos_sleep_ns(JOIN_OUTLIVE_NS);
    }

    void t_join_timeout()
    {
        auto w = kos::thread::spawn(join_slow, nullptr, "jslo", 10);
        TAP_CHECK(w.valid());
        uint64_t const t0 = kos_clock_now();
        int const rc = w.join(JOIN_TIMEOUT_US);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(rc == -KOS_ETIMEDOUT); // the target is still running, and nothing waits on it
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(waited_us >= JOIN_TIMEOUT_US);
        // The expiry cleared the wait edge, so the target's exit sweep finds nothing to
        // wake and this is a FRESH park. It must still be woken by that same exit.
        TAP_CHECK(w.join() == 0);
    }

    // --- Tasks: the handle codec, the creator gate, and the group kill ----------
    //
    // A task handle carries a generation over a BIASED index, so the all-zero word is
    // KOS_TASK_NONE and no live task is ever named by it. The gate is CREATORSHIP, which
    // cannot be witnessed from one thread, so what is checked here is what one thread can
    // see: every refusal the codec produces, and that a hold, once dropped, names nothing.
    void t_task_handles()
    {
        // The out-pointer is validated BEFORE the group exists: a null one is malformed and a
        // misaligned one would take a privileged store the kernel must not make. Checked first,
        // because a mint that cannot deliver its handle leaves a task nothing can name.
        TAP_CHECK(kos_task_create(nullptr, 0, nullptr) == -KOS_EINVAL);
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        TAP_CHECK(task != KOS_TASK_NONE); // the bias is what makes this assertion possible
        // The two words nothing can mint: the sentinel, and a generation the slot never held.
        TAP_CHECK(kos_task_kill(KOS_TASK_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(task ^ 0xFFFF0000u) == -KOS_EBADF);
        // An out-of-range index, whatever generation rides it.
        TAP_CHECK(kos_task_kill(0x0000FFFFu) == -KOS_EBADF);
        // AN IMPLICIT TASK IS UNNAMEABLE, and this is the arm that matters most here: idle's
        // task is created first (kmain makes idle before root) and root's second, so slots 0
        // and 1 hold them and the biased codec names those two handles 1 and 2 at generation
        // 0. Neither slot is ever freed, so the generation cannot drift. Both carry the KERNEL
        // domain, the whole arena at R|W, so a handle that resolved would let this thread
        // spawn a child INTO it and hand an unprivileged thread the arena.
        TAP_CHECK(kos_task_kill(1u) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(2u) == -KOS_EBADF);
        struct kos_thread_params ip = {};
        ip.entry = join_probe;
        ip.name = "timp";
        ip.prio = 10;
        ip.task = 2u; // root's own implicit task
        kos_thread_t ih = KOS_THREAD_NONE;
        TAP_CHECK(kos_thread_spawn(&ip, &ih) == -KOS_EBADF);
        TAP_CHECK(ih == KOS_THREAD_NONE);

        // A group holding no thread still RESERVES its slot: an implicit task minted by the
        // spawn below must not be handed the slot `task` is sitting in, or that spawn would
        // overwrite the creator tag and `task` would stop naming anything.
        auto probe = kos::thread::spawn(join_probe, nullptr, "trsv", 10);
        if (probe.valid())
        {
            TAP_CHECK(probe.join(JOIN_GENEROUS_US) == 0);
        }
        TAP_CHECK(kos_task_kill(task) == 0);
        // The kill DROPPED the hold, and an empty group with no hold is a free slot, so the
        // handle now names nothing. Killing twice is not idempotent, and must not be.
        TAP_CHECK(kos_task_kill(task) == -KOS_EBADF);
    }

    // The CREATOR GATE, both halves, and it takes a second thread to witness at all: only the
    // thread that made a group may seat a member into it or end it. Possession of the handle is
    // deliberately not enough, because the codec is guessable and a resolvable handle would
    // be an authority anyone could forge.
    //
    // The stranger reports over an ENDPOINT rather than a shared global, and root receives with
    // a DEADLINE: a stranger that never answers must fail this arm rather than hang it.
    void task_stranger(void* arg) // caps: E@1
    {
        kos_task_t const t = static_cast<kos_task_t>(reinterpret_cast<uintptr_t>(arg));
        unsigned char answer[2] = {0u, 0u};
        struct kos_thread_params p = {};
        p.entry = join_probe;
        p.name = "tsmb";
        p.prio = 10;
        p.task = t;
        kos_thread_t h = KOS_THREAD_NONE;
        if (kos_thread_spawn(&p, &h) == -KOS_EPERM)
        {
            answer[0] = 1u;
        }
        if (kos_task_kill(t) == -KOS_EPERM)
        {
            answer[1] = 1u;
        }
        (void)kos_send(KOS_SPAWN_DELEGATED_CAP0, answer, sizeof(answer));
        kos_exit(0);
    }

    void t_task_creator_gate()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no endpoint slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{ep, CH_FULL}};
        auto stranger = kos::thread::spawn(
            task_stranger, reinterpret_cast<void*>(static_cast<uintptr_t>(task)), "tstr", 10,
            KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
            caps, 1);
        if (not stranger.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(ep);
            tap::skip("pool too small");
            return;
        }
        unsigned char answer[2] = {0u, 0u};
        struct kos_recv_timed_opts opts;
        opts.timeout_us = JOIN_GENEROUS_US;
        opts.info.badge = 0u;
        opts.info.reply_cap = KOS_CAP_NONE;
        TAP_CHECK(kos_recv_timed(ep, answer, sizeof(answer), &opts) == 2);
        TAP_CHECK(answer[0] == 1u); // a stranger cannot seat a member
        TAP_CHECK(answer[1] == 1u); // nor end the group
        TAP_CHECK(stranger.join(JOIN_GENEROUS_US) == 0);
        // Still ours, so still killable: the refusals above cost the group nothing.
        TAP_CHECK(kos_task_kill(task) == 0);
        TAP_CHECK(kos_handle_close(ep) == 0);
    }

    // A member's memory is its TASK's, so a member bringing its own data grant is refused
    // rather than having it silently dropped; and a task nobody created cannot be joined.
    void t_task_member_refusals()
    {
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        void* const blk = kos_ram_alloc(64);
        TAP_CHECK(blk != nullptr);
        struct kos_thread_params p = {};
        p.entry = join_probe;
        p.name = "tmem";
        p.prio = 10;
        p.task = task;
        p.mem_base = blk;
        p.mem_size = 64;
        kos_thread_t h = KOS_THREAD_NONE;
        TAP_CHECK(kos_thread_spawn(&p, &h) == -KOS_EINVAL);
        TAP_CHECK(h == KOS_THREAD_NONE);
        // Same spawn against a handle no slot answers.
        p.mem_base = nullptr;
        p.mem_size = 0;
        p.task = KOS_TASK_NONE ^ 0x0000FFFFu; // a real generation over no index
        TAP_CHECK(kos_thread_spawn(&p, &h) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(task) == 0);
    }

    // THE group kill, end to end. The member parks on a semaphore nothing ever posts, which
    // is the shape a cooperative cancel could never reach: sem_wait has no error return to
    // carry a reason. The kill breaks that park anyway and the kernel ends the thread at its
    // next syscall, so the JOIN is what proves it died rather than merely being marked.
    void task_member(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
        // Unreachable: nothing posts that semaphore, and a cancelled thread does not return
        // from the syscall that follows.
        kos_exit(1);
    }

    void t_task_group_kill()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
        auto member = kos::thread::spawn(task_member, nullptr, "tmbr", 10, KOS_POLICY_FIFO, 0,
                                         /*privileged=*/false, nullptr, 0, nullptr, 0,
                                         nullptr, 0, caps, 1, /*authority=*/0,
                                         /*cap_dest=*/nullptr, task);
        if (not member.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        TAP_CHECK(kos_task_kill(task) == 0);
        // 0, not ETIMEDOUT: the member has to be GONE, not just marked.
        TAP_CHECK(member.join(JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // --- Slay: the cleanup window a kill leaves and a slay denies ---------------
    // THE arm for docs/design-kill-and-slay.md, and the only one in the tree that witnesses
    // the death point on real silicon: everything the host seam can see is the seam's
    // ARGUMENTS, never a resumed context.
    //
    // The window is a PLAIN MEMORY WRITE and it has to be: the cooperative death point is
    // the next syscall ENTRY, so any syscall placed here would BE that point and a killed
    // thread would look exactly like a slain one.
    Atomic<int, Order::RELAXED> g_slay_window{0};

    // caps: done@1, park@2. Announces itself, then parks on a semaphore NOTHING ever posts,
    // so the only way past that wait is a cancellation breaking the park.
    void slay_window_worker(void*)
    {
        kos_sem_post(CH_DONE);
        kos_sem_wait(2);
        g_slay_window = g_slay_window + 1;
        kos_exit(0); // never returns: the kernel ends the thread at this syscall's entry
    }

    // The worker outranks root (prio 10 against KICKOS_PRIO_MIN + 1), so its post wakes root
    // without preempting it and root cannot run again until the worker PARKS. That is what
    // makes "the target was parked when the request was made" a fact rather than a hope --
    // and without it both arms below pass vacuously, the worker having died at the entry to
    // a wait it never reached.
    bool stage_a_parked_slay_worker(kos::thread::Handle* out, kos_cap_t park)
    {
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        *out = kos::thread::spawn_caps(slay_window_worker, nullptr, "slay", 10, caps, 2);
        if (not out->valid())
        {
            return false;
        }
        wait_n(1);
        return true;
    }

    void t_thread_slay_window()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        g_slay_window = 0;
        // LEG 1, the control. A kill breaks the park and the target returns to userspace for
        // exactly as long as it takes to reach its next syscall.
        kos::thread::Handle killed;
        if (not stage_a_parked_slay_worker(&killed, park))
        {
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        TAP_CHECK(killed.kill() == 0);
        TAP_CHECK(killed.join(JOIN_GENEROUS_US) == 0);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 1); // the window is real, so leg 2 is not vacuous

        // LEG 2, the subject. Same body, same park, same parent: only the verb differs.
        kos::thread::Handle slain;
        if (not stage_a_parked_slay_worker(&slain, park))
        {
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // 0, not -KOS_ETIMEDOUT: the call WAITS, and gone is what it returns.
        TAP_CHECK(slain.slay(JOIN_GENEROUS_US) == 0);
        int const slay_window_after = g_slay_window;
        TAP_CHECK(slay_window_after == 1); // unchanged: it executed no further user instruction
        // Gone means gone, and the join is the independent witness of it.
        TAP_CHECK(kos_thread_join(slain.id(), JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // The gate, which is the kill gate unchanged: slay reaches exactly the set kill reaches.
    void slay_gate_probe(void*) // caps: done@1
    {
        // Root is nobody's child (kill_tag_of never answers KILL_TAG_NONE), so this is the
        // parenthood arm and not the self arm.
        char c = 'x';
        if (kos_thread_slay(ROOT_THREAD, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_thread_slay_gate()
    {
        log_reset();
        TAP_CHECK(kos_thread_slay(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_slay(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        // Not -KOS_EDEADLK as join answers: ending yourself is kos_exit, and this call has
        // to be able to return to its caller.
        TAP_CHECK(kos_thread_slay(ROOT_THREAD, JOIN_GENEROUS_US) == -KOS_EINVAL);
        // An EXITED-but-unreclaimed slot, which join deliberately ACCEPTS and every cancel
        // deliberately refuses: there is nothing left in it to condemn.
        auto probe = kos::thread::spawn(join_probe, nullptr, "slgn", 10);
        TAP_CHECK(probe.valid());
        TAP_CHECK(probe.join(JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_thread_slay(probe.id(), JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}}; // done@1, lock@2
        auto s = kos::thread::spawn_caps(slay_gate_probe, nullptr, "slst", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // parenthood, and there is no capability to delegate it with
    }

    // --- Slay: the group form ---------------------------------------------------
    // 0 here means a condition no other call in the ABI waits on: the group is EMPTY. That
    // is what makes the task form worth having, since a group slay that could only ever
    // answer -KOS_ETIMEDOUT would have an unreachable success case.
    void t_task_slay_group()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        g_slay_window = 0;
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        auto member = kos::thread::spawn(slay_window_worker, nullptr, "tsly", 10,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                         nullptr, 0, nullptr, 0, caps, 2, /*authority=*/0,
                                         /*cap_dest=*/nullptr, task);
        if (not member.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        wait_n(1); // the member outranks root, so it is PARKED once this returns
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == 0);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0);          // no member got a cleanup window
        TAP_CHECK(member.join(JOIN_GENEROUS_US) == 0); // and the member really is gone
        // The hold went with the wait, so the handle names nothing: a second call cannot
        // resolve it, which is the same shape kos_task_kill leaves behind.
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    void task_slay_stranger(void* arg) // caps: done@1, lock@2
    {
        kos_task_t const task = static_cast<kos_task_t>(reinterpret_cast<uintptr_t>(arg));
        char c = 'x';
        if (kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_task_slay_gate()
    {
        log_reset();
        TAP_CHECK(kos_task_slay(KOS_TASK_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, &task) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::spawn_caps(task_slay_stranger,
                                         reinterpret_cast<void*>(static_cast<uintptr_t>(task)),
                                         "tsst", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // creatorship, exactly as kos_task_kill takes it
        // An EMPTY group answers 0 with nothing to slay: dropping the creator's hold is the
        // whole of the work, and a park here would never be woken.
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(s.join(JOIN_GENEROUS_US) == 0);
    }

    // --- Slay: condemned is not yet gone ----------------------------------------
    // The middle guarantee level, which is the whole reason there are two calls and three
    // returns. It needs a victim that CANNOT be scheduled while the deadline runs, so a
    // higher-priority thread holds the CPU across it: that is the starvation hazard the
    // timeout exists to make visible instead of hiding in an unbounded park.
    constexpr uint32_t SLAY_TIMEOUT_US = 4000;
    constexpr uint64_t SLAY_HOG_NS = 60000000ull; // 15x the deadline

    // Published so the caller can tell a window it still holds from one it has already
    // spent; 0 until the hog has actually run, which a spawn does not guarantee.
    // The stamp is the low 32 bits of the start, never the 64-bit deadline: a 60 ms window
    // inside the 4.29 s wrap leaves the elapsed arithmetic unambiguous.
    Atomic<uint32_t, Order::RELAXED> g_hog_start_ns{0};

    void slay_hog(void*) // caps: none
    {
        uint64_t const start = kos_clock_now();
        uint64_t const until = start + SLAY_HOG_NS;
        g_hog_start_ns = static_cast<uint32_t>(start);
        while (kos_clock_now() < until)
        {
        }
    }

    // The hog must have more window left than the deadline the caller is about to arm, or
    // the slay measures nothing. Twice the deadline, so the margin is not itself the race.
    bool hog_window_open()
    {
        uint32_t const start = g_hog_start_ns;
        if (start == 0)
        {
            // Not yet run is the healthy case and the opposite of spent: the caller
            // outranks the hog, which first runs when this thread parks inside the slay,
            // so the whole window is still ahead.
            return true;
        }
        uint32_t const window = static_cast<uint32_t>(SLAY_HOG_NS);
        uint32_t const elapsed = static_cast<uint32_t>(kos_clock_now()) - start;
        if (elapsed >= window)
        {
            return false;
        }
        return (window - elapsed) > (2u * SLAY_TIMEOUT_US * 1000u);
    }

    void t_thread_slay_timeout()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        g_slay_window = 0;
        kos::thread::Handle victim;
        if (not stage_a_parked_slay_worker(&victim, park))
        {
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // Outranks the victim, so nothing the slay does can get the victim onto the CPU
        // until this thread is finished. Spawned AFTER the victim is staged, because a
        // spawn is not a barrier and this one would otherwise hog the staging itself.
        g_hog_start_ns = 0; // a previous arm's window must not read as this one's
        auto hog = kos::thread::spawn(slay_hog, nullptr, "shog", 11);
        if (not hog.valid())
        {
            TAP_CHECK(victim.slay(JOIN_GENEROUS_US) == 0);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // CONDEMNED, not gone: the redirect is armed and irrevocable, and the sweep has not
        // run because the victim has not been given the CPU to run it on.
        // The window runs from the hog's first run, not from this call, and the caller can
        // lose all of it between the two. Under an interrupt-driven console it does: the
        // caller blocks in the console, the hog spends its window, and the victim then dies
        // at once because nothing outranks it. slay returning 0 there is correct, so
        // asserting the timeout without this check measures nothing.
        // A spent window is recoverable, and skipping instead would leave the starvation
        // guarantee unexercised under the console that broke it. The hog has exited, so join
        // it and stage a fresh one.
        for (int attempt = 0; attempt < 2 and not hog_window_open(); attempt++)
        {
            (void)hog.join(JOIN_GENEROUS_US);
            g_hog_start_ns = 0;
            hog = kos::thread::spawn(slay_hog, nullptr, "shog", 11);
            if (not hog.valid())
            {
                break;
            }
        }
        if (not hog.valid() or not hog_window_open())
        {
            if (hog.valid())
            {
                (void)hog.join(JOIN_GENEROUS_US);
            }
            (void)victim.slay(JOIN_GENEROUS_US);
            (void)kos_handle_close(park);
            tap::skip("hog window spent before the slay -- starvation not established");
            return;
        }
        TAP_CHECK(victim.slay(SLAY_TIMEOUT_US) == -KOS_ETIMEDOUT);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0); // and it never got its window either
        // IRREVOCABLE is the claim, so the same handle must reach GONE with no second
        // request: the timeout gave up on the wait, never on the death.
        TAP_CHECK(kos_thread_join(victim.id(), JOIN_GENEROUS_US) == 0);
        int const slay_window_after_join = g_slay_window;
        TAP_CHECK(slay_window_after_join == 0);
        TAP_CHECK(hog.join(JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // --- Self-grant, and the region budget that bounds it ----------------------
    // Exercises the REFUSAL at the region-budget ceiling: the call must fail LOUDLY
    // (-KOS_ENOMEM), not truncate the region set.
    //
    // Runs in an unprivileged CHILD, not in root: a privileged caller's self-grants are
    // answered "already reachable" without spending a descriptor, so the ceiling would
    // be unreachable.
    //
    // Each descriptor is bought with kos_ram_alloc(1), the SMALLEST region this
    // backend can describe (the allocator rounds up to arch_ram_region_size). The
    // blocks are not reclaimed (arch_ram_alloc is a bump allocator with no free), so
    // the bound below is also what keeps the leak small.
    constexpr int SG_MAX = 12; // > KICKOS_MPU_MAX_REGIONS, so the loop must end refused
    Atomic<int, Order::RELAXED> g_sg_ok{0};       // descriptors accepted before the ceiling
    Atomic<int32_t, Order::RELAXED> g_sg_refusal{0}; // the code that ended the loop
    Atomic<int32_t, Order::RELAXED> g_sg_badsize{0};
    Atomic<int, Order::RELAXED> g_sg_readback{-1};

    void selfgrant_worker(void*)
    {
        // Size-0 refusal costs no arena and no descriptor. The address is a valid
        // stack local, so the refusal is about the SIZE alone.
        int probe = 0;
        g_sg_badsize = kos_mem_self_grant(&probe, 0);
        for (int i = 0; i < SG_MAX; i++)
        {
            void* p = kos_ram_alloc(1);
            if (p == nullptr)
            {
                g_sg_refusal = 0; // arena, not budget: the parent skips rather than fails
                break;
            }
            int32_t const rc = kos_mem_self_grant(p, 1);
            if (rc != 0)
            {
                g_sg_refusal = rc;
                break;
            }
            // Touch the page just granted: an ungranted write from an unprivileged
            // thread faults, so reaching the readback is the positive half.
            *static_cast<volatile int*>(p) = 0x5A5A + i;
            g_sg_readback = *static_cast<volatile int*>(p) - i;
            g_sg_ok = g_sg_ok + 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_selfgrant()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(selfgrant_worker, nullptr, "sgW", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        if (g_sg_refusal == 0)
        {
            tap::skip("arena too small to reach the region ceiling");
            return;
        }
        // Grouped: each TAP_CHECK carries __FILE__ plus its stringified condition as
        // rodata.
        int const sg_ok = g_sg_ok;
        int32_t const sg_refusal = g_sg_refusal;
        TAP_CHECK(sg_ok > 0 and sg_refusal == -KOS_ENOMEM);
        int const sg_readback = g_sg_readback;
        int32_t const sg_badsize = g_sg_badsize;
        TAP_CHECK(sg_readback == 0x5A5A and sg_badsize == -KOS_EINVAL);
    }

    // --- Self-grant of a non-power-of-two region ------------------------------
    // Wherever arch_ram_region_size is granular rather than pow2 (a min-region-0
    // backend like nRF51/LX6, and every base+limit MPU), three granules round to three
    // granules, so rsz - 1 is not an alignment mask, and a block kos_ram_alloc handed
    // out must still self-grant. The sizes must be granule-derived: a constant only
    // discriminates at one granule.
    // Consecutive 3-granule blocks step through the granule residues of 4 granules, so
    // within three blocks one base is not 4-granule aligned, exactly the base an
    // ungated pow2 mask check refuses. On a pow2 backend every base is naturally
    // aligned and this is one ordinary grant.
    //
    // Registered BEFORE domain_share, not with mem_self_grant: on microbit the
    // pool cannot host a worker by the time the budget test runs, and this probe
    // is only discriminating on exactly such a no-MPU board.
    Atomic<int32_t, Order::RELAXED> g_sgnp_rc{-1};
    Atomic<int, Order::RELAXED> g_sgnp_ran{0};
    void sgnp_worker(void*)
    {
        size_t const g = discover_granule();
        if (g == 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        size_t const want = 3u * g;
        void* pick = nullptr;
        for (int i = 0; i < 3; i++)
        {
            void* p = kos_ram_alloc(want);
            if (p == nullptr)
            {
                break;
            }
            if (pick == nullptr)
            {
                pick = p;
            }
            if ((reinterpret_cast<uintptr_t>(p) & (4u * g - 1u)) != 0)
            {
                pick = p;
                break;
            }
        }
        if (pick != nullptr)
        {
            g_sgnp_rc = kos_mem_self_grant(pick, want);
            g_sgnp_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    // --- Which region-encoding mode is live on this board ----------------------
    // The bump allocator's step for a 3-granule request IS the mode: a base+limit
    // backend reserves 3 granules, a pow2 backend rounds to 4.
    void t_region_mode()
    {
        size_t const g = discover_granule();
        if (g == 0)
        {
            tap::skip("arena too small to discover the granule");
            return;
        }
        void* p = kos_ram_alloc(3u * g);
        void* q = kos_ram_alloc(g);
        if (p == nullptr or q == nullptr)
        {
            tap::skip("arena too small for the mode probe blocks");
            return;
        }
        uintptr_t const a = reinterpret_cast<uintptr_t>(p);
        uintptr_t const b = reinterpret_cast<uintptr_t>(q);
        size_t const step = static_cast<size_t>(b - a);
        // Userspace cannot separate the granular enforcing mode from the no-MPU mode:
        // both allocate granule multiples. The report names the observed shaping only.
        if (step == 3u * g)
        {
            tap::diag("region shaping: GRANULE-MULTIPLE (granule %lu, 3-granule request reserved %lu)",
                      static_cast<unsigned long>(g), static_cast<unsigned long>(step));
        }
        else if (step == 4u * g)
        {
            tap::diag("region shaping: POWER-OF-TWO (granule %lu, 3-granule request reserved %lu)",
                      static_cast<unsigned long>(g), static_cast<unsigned long>(step));
        }
#if defined(KICKOS_MPU_MIN_REGION_CFG) and defined(KICKOS_MPU_REGION_POW2_CFG)
        // Independent oracle: cmake scraped these two literals textually out of the
        // backend .cc this image links, so the expectation shares no source with the
        // arch_mpu_region_pow2() call the allocator made.
        // Undefined on the sim, which never reaches that scrape; the #else branch is
        // the weaker self-consistency check.
        size_t expect = 4u * g;
        if (KICKOS_MPU_MIN_REGION_CFG == 0)
        {
            expect = 3u * g; // no MPU: granule multiples
        }
        else if (KICKOS_MPU_REGION_POW2_CFG == 0)
        {
            expect = 3u * g;
        }
        TAP_CHECK(step == expect);
        if (KICKOS_MPU_MIN_REGION_CFG != 0)
        {
            TAP_CHECK(g == static_cast<size_t>(KICKOS_MPU_MIN_REGION_CFG));
        }
#else
        TAP_CHECK(step == 3u * g or step == 4u * g);
#endif
    }

    void t_selfgrant_nonpow2()
    {
        // Unprivileged + AUTH_MEMORY, like t_selfgrant: a privileged caller is
        // answered "already reachable" before the geometry is ever examined.
        kos_cap_grant caps[] = {{g_done, CH_FULL}}; // done@1
        auto w = kos::thread::spawn_caps(sgnp_worker, nullptr, "sgNP", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        if (g_sgnp_ran == 0)
        {
            tap::skip("arena too small for the probe blocks");
            return;
        }
        int32_t const sgnp_rc = g_sgnp_rc;
        TAP_CHECK(sgnp_rc == 0);
    }
}

// The suite drives the authority gates from root, so it keeps five of the six. Not
// KOS_AUTH_PSTATE: retuning the core clock from root would retime every deadline the
// timing tests assert (see t_cpu_clock_set), so that arm runs in a worker instead.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_PINMUX
                     | KOS_AUTH_IRQ | KOS_AUTH_CONSOLE);

int main(int, char**)
{
    kos_sem_create(1, &g_lock);
    kos_sem_create(0, &g_done);
    tap::set_after_failure(done_reset);

// Region 1: everything down to the #undef below. Moving an arm across that boundary,
// adding one or deleting one has to move the matching per-part floor in this app's
// CMakeLists, which asserts the two floors still sum to the whole suite.
#if KICKOS_SELFTEST_PART == 0 || KICKOS_SELFTEST_PART == 1
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    // Core scheduler / sync / time: no test-only syscalls, runs on every board.
    TAP_ADD("svc_roundtrip", t_svc);
    TAP_ADD("fifo_order", t_fifo);
    TAP_ADD("preempt_on_ready", t_preempt);
    TAP_ADD("cpu_clock_hz", t_cpu_clock_hz);
    TAP_ADD("periph_clock_hz", t_periph_clock_hz);
    TAP_ADD("byte_ring", t_byte_ring); // pure logic: unconditional, every board
    TAP_ADD("console_crlf", t_console_crlf); // pure logic: unconditional, every board
    TAP_ADD("cap_dest", t_cap_dest);
    TAP_ADD("pinmux_set", t_pinmux_set);
    TAP_ADD("cpu_clock_set", t_cpu_clock_set);
    TAP_ADD("rr_interleave", t_rr);
    TAP_ADD("sleep_order", t_sleep);
    TAP_ADD("multi_wait", t_multi);
    TAP_ADD("sem_destroy", t_sem_destroy);
    TAP_ADD("sem_destroy_quiescent", t_sem_destroy_busy);
    TAP_ADD("sem_raii", t_sem_raii);
    // PI-mutex capability: production syscalls only, so runs on every board.
    TAP_ADD("mutex_basic", t_mutex_basic);             // H1 mutual exclusion
    TAP_ADD("mutex_pi_donation", t_mutex_pi);          // H2/H4/H8 boost + revert
    TAP_ADD("mutex_chain_boost", t_mutex_chain);       // H5 chained boost
    TAP_ADD("mutex_owner_died", t_mutex_owner_died);   // H7/R3 exit-while-owning
    TAP_ADD("mutex_deadlock", t_mutex_deadlock);       // H6 self + cycle refusal
    TAP_ADD("mutex_close_owned", t_mutex_close_owned); // R2 close-of-owned refused
    TAP_ADD("mutex_multi_held", t_mutex_multi_held);   // H3 recompute vs restore-to-base
    TAP_ADD("mutex_unlock_errors", t_mutex_unlock_errors); // non-owner / unlocked -> -KOS_EPERM
    TAP_ADD("mutex_owner_died_nowaiter", t_mutex_owner_died_nowaiter); // R3 no-waiter branch
    TAP_ADD("mutex_deleg_refcount", t_mutex_deleg_refcount); // child close, parent still locks
    // Endpoint IPC: production syscalls, so runs on every board.
    TAP_ADD("endpoint_rendezvous", t_endpoint_rendezvous); // both orderings + zero-len + truncation
    TAP_ADD("endpoint_reject", t_endpoint_reject);         // F4 oversize + bad cap
    TAP_ADD("endpoint_rights", t_endpoint_rights);         // send needs SIGNAL, recv needs WAIT
    TAP_ADD("endpoint_epipe", t_endpoint_epipe);           // parked sender woken on last WAIT close
    TAP_ADD("endpoint_dead", t_endpoint_dead);             // F1 dead endpoint: send refused, no park
    TAP_ADD("endpoint_send_timeout", t_endpoint_send_timeout); // timed send expires; untimed still parks
    TAP_ADD("recv_timeout", t_recv_timeout);                   // timed recv expires with nobody sending
    TAP_ADD("timed_arg_refusals", t_timed_arg_refusals);       // null/misaligned opts, oversize packed send_len
    TAP_ADD("call_timeout_pending", t_call_timeout_pending);   // timed call expires on send_waiters
    TAP_ADD("call_timeout_revert", t_call_timeout_revert);     // ... and that unwind reverts the D2 boost
    TAP_ADD("call_timeout_reply", t_call_timeout_reply);       // ... and in CALL_REPLY_WAIT, via the SLOW path
    TAP_ADD("reply_stale_caller", t_reply_stale_caller);       // reply to a timed-out caller: -KOS_ESRCH
    TAP_ADD("reply_abandoned_cap", t_reply_abandoned_cap);     // ... and while that caller is in a SECOND call
    TAP_ADD("call_infoless_revert", t_call_infoless_revert); // info-less bounce reverts the D2 boost
    TAP_ADD("call_close_reply", t_call_close_reply);         // close-instead-of-reply EPIPEs + yields
    TAP_ADD("call_happy", t_call_happy);                     // request delivered + reply in-place (fast+slow)
    TAP_ADD("call_from_root", t_call_from_root);             // root is a pool slot, so it may call (fast+slow)
    TAP_ADD("call_truncation", t_call_truncation);           // request + reply datagram clamp
    TAP_ADD("call_double_reply", t_call_double_reply);       // one-shot cap -> second reply -KOS_EBADF
    TAP_ADD("call_server_death", t_call_server_death);       // die mid-xact -> caller EPIPE (teardown arm)
    TAP_ADD("call_prepop_death", t_call_prepop_death);       // die pre-pop -> caller EPIPE (recv_holders 0)
    TAP_ADD("call_donation", t_call_donation);               // D1 donation keeps the spoiler off the xact
    TAP_ADD("call_donation_hold", t_call_donation_hold);     // D3: a reply donor survives an unrelated recompute
    TAP_ADD("call_donation_slow", t_call_donation_slow);     // D3: same, via the recv-side mint
    TAP_ADD("call_donation_pending", t_call_donation_pending); // D3: a SEND_WAIT donor does too
#undef TAP_ADD
// Region 2.
#if KICKOS_SELFTEST_PART == 0 || KICKOS_SELFTEST_PART == 2
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("bus_device_slots", t_bus_device_slots); // per-device profiles do not clobber
    TAP_ADD("uart_service", t_uart_service);         // the UART wire ABI over the rings
#endif
    TAP_ADD("endpoint_crossdomain", t_endpoint_crossdomain); // F5 cross-domain copy + delegation
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("endpoint_bound", t_endpoint_bound); // bound-check: bad recv/send buffer refused
#endif
    // Console handover mechanism: production syscalls, every board.
    TAP_ADD("cap_index0", t_cap_index0);              // B3 index-0 reservation + FIRST_DYNAMIC floor
    TAP_ADD("cap_chunk_span", t_cap_chunk_span);        // the segmented index decode
    TAP_ADD("cap_gen_reuse", t_cap_gen_reuse);          // the cap-gen half of the codec
    TAP_ADD("cap_child_width", t_cap_child_width);      // a child gets the child width
    TAP_ADD("cap_reply_bound_fast", t_cap_reply_bound_fast); // the bound at the fastpath probe
    TAP_ADD("cap_reply_bound_slow", t_cap_reply_bound_slow); // the same, via the recv-side scan
    TAP_ADD("cap_reply_release_close", t_cap_reply_release_close); // close consumes a reply cap
    TAP_ADD("cap_reply_slot_reuse", t_cap_reply_slot_reuse);       // a dead server's slot is clean
    TAP_ADD("console_publish_priv", t_console_publish); // D3 privileged-only + bad-cap reject
    TAP_ADD("shutdown_priv", t_shutdown_denied);        // KOS_SYS_SHUTDOWN privileged-only
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("reboot_priv", t_reboot_denied);            // KOS_SYS_REBOOT: refusal arm only
#endif
    TAP_ADD("writable_global", t_writable_global);      // out-buffer in an app global
    TAP_ADD("authority_cap", t_authority_cap);          // authority word: both arms of the gates
    TAP_ADD("periph_enable_unheld", t_periph_enable_unheld); // possession is the whole periph-enable gate
    TAP_ADD("periph_reg_write_unheld", t_periph_reg_write_unheld); // same gate + the offset bound + the chip refusal
#if KICKOS_ARCH_SIM
    TAP_ADD("periph_reg_write_mask", t_periph_reg_write_mask); // allowlist match + the per-entry value mask
#endif
    TAP_ADD("privileged_spawn_refused", t_privileged_spawn_refused); // no privilege minting after boot
    TAP_ADD("thread_join", t_thread_join);   // parked join, unreclaimed exit, the refusals
    TAP_ADD("join_stale_gen", t_join_stale_gen); // a reclaimed slot: the generation branch
    TAP_ADD("join_timeout", t_join_timeout); // a target that outlives its deadline
    TAP_ADD("task_handles", t_task_handles);  // the task handle codec and the dropped hold
    TAP_ADD("task_member_refusals", t_task_member_refusals); // a member brings no memory
    TAP_ADD("task_creator_gate", t_task_creator_gate); // a stranger may neither seat nor kill
    TAP_ADD("task_group_kill", t_task_group_kill); // a kill that reaches a semaphore park
    TAP_ADD("thread_slay_window", t_thread_slay_window); // kill leaves the window, slay denies it
    TAP_ADD("thread_slay_gate", t_thread_slay_gate);     // parenthood, self, and a spent slot
    TAP_ADD("thread_slay_timeout", t_thread_slay_timeout); // condemned is not yet gone
    TAP_ADD("task_slay_group", t_task_slay_group); // 0 means the GROUP is empty
    TAP_ADD("task_slay_gate", t_task_slay_gate);   // creatorship, and an already-empty group
#if defined(KICKOS_ENABLE_SELFTEST)
    // Need the software-inject syscall (compiled out of the production ABI).
    TAP_ADD("irq_thread_ctx", t_irq);
    TAP_ADD("irq_as_event", t_irqdrv);
    TAP_ADD("irq_mask_coalesce", t_irq_mask);
    TAP_ADD("irq_discard", t_irq_discard);
    TAP_ADD("irq_autorearm", t_irq_autorearm);
    TAP_ADD("irq_phantom_wake", t_irq_phantom);
    TAP_ADD("irq_ownership", t_irq_ownership);
    TAP_ADD("irq_spurious", t_irq_spurious);
    TAP_ADD("irq_stale_register", t_irq_stale_register);
    TAP_ADD("irq_claim_gate", t_irq_claim_gate);
    TAP_ADD("irq_reclaim", t_irq_reclaim);
#endif
    TAP_ADD("caller_stack", t_caller_stack); // caller-owned stack API (no test-only syscalls)
    // Here, not beside mem_self_grant: see the run-order note at t_selfgrant_nonpow2.
    TAP_ADD("mem_self_grant_nonpow2", t_selfgrant_nonpow2); // non-pow2 region self-grants
    TAP_ADD("region_mode", t_region_mode);                  // which region-encoding mode is live
    TAP_ADD("domain_share", t_domain_share); // two threads share one memory domain
    TAP_ADD("mmio_grant", t_mmio_grant);     // MMIO-grant boundary: privileged-only + encodable-only
#if KICKOS_HAVE_MPU
    TAP_ADD("stackbase_arena", t_stackbase_arena); // unprivileged out-of-arena stack_base refused
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("grant_reserved", t_grant_reserved);   // Rule 7: overlap matrix + RAM/DEV admission (probe syscall)
    TAP_ADD("dev_window_exclusive", t_dev_window_exclusive); // one holder per DEV window (-KOS_EBUSY)
#endif
#endif
    TAP_ADD("confused_deputy", t_confused_deputy); // readable-buffer/name floor (accept rodata, reject bogus)
    // Last, deliberately: the blocks it buys are never returned (bump allocator), so
    // running it earlier would spend arena the tests above still need on a small board.
    TAP_ADD("mem_self_grant", t_selfgrant); // explicit grant + the loud budget ceiling
#undef TAP_ADD

    // Every test joins its workers, so main returns as the last live thread:
    // the failure count becomes the process exit status (0 == all passed).
    return tap::run_all();
}
