// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS self-test (unprivileged userspace, C++), the CI gate: every verification
// bullet is a TAP arm that self-asserts its invariant over the console (tests/tap).
// Ordering-sensitive arms assert on a semaphore-locked event log, never on console text.

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

#include "tap.h"

// The chip's own constants. A sim build ships none (same guard as config/board.h), so
// anything read from here needs a fallback.
#if defined(__has_include) and __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// Base of the IRQ arms' nine-line block, BASE+0 through BASE+8, claimed by no other holder
// in this image. A board that defines no KICKOS_IRQ_SOFT_ONLY_BASE also takes BASE+9 and
// BASE+10 for the discard and irq-context arms, so eleven lines in all. A chip whose own
// drivers sit in that span must move the base.
#ifndef KICKOS_SELFTEST_IRQ_BASE
#define KICKOS_SELFTEST_IRQ_BASE 6
#endif

// Which region of the registration list at the bottom of this file to register: 0 (the
// default) is all of it, 1 and 2 are the two contiguous regions the 64 KiB FLASH parts
// build as separate images. TAP_ADD is REDEFINED at the boundary, so an arm belongs to the
// part its line sits in.
#ifndef KICKOS_SELFTEST_PART
#define KICKOS_SELFTEST_PART 0
#endif

// Unevaluated operand: counts as a use for -Wunused-function without emitting the body.
#define TAP_ELIDE(fn) ((void)sizeof(&(fn)))

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    kos_cap_t g_done = KOS_CAP_NONE; // shared completion counter (MAIN's cap; delegated to workers)
    kos_cap_t g_lock = KOS_CAP_NONE; // binary semaphore = mutex over the event log (MAIN's cap)

    // Well-known child cap indices: a fresh child table has cap-gen 0, so delegated cap i
    // lands at index i+1 (index 0 reserved). Every spawn must delegate in exactly this order.
    constexpr int CH_DONE = 1;  // delegated FIRST to every worker
    constexpr int CH_LOCK = 2;  // delegated SECOND (logging workers only)
    constexpr int CH_AUX = 3;
    constexpr int CH_READY = 2; // IRQ-driver tests
    constexpr int CH_IRQ = 3;   // IRQ-driver tests
    constexpr uint8_t CH_FULL =
        KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // root's region set is [app code RX, app static data RW, its own stack], and
    // kos_ram_alloc grants the caller nothing: a test that must touch its own allocation
    // asks with kos_mem_self_grant.

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

    // Call ONLY after a failure: between every arm it churns a cap slot 90 times over and
    // starves the later spawns on the smallest board.
    void done_reset()
    {
        kos_sem_destroy(g_done);
        kos_sem_create(0, &g_done);
    }

    // Staging gate: every worker of a test must exist before ANY of them runs. It MUST be
    // its own semaphore: gating on the event-log mutex hands the token straight to the next
    // waiter and the workers ping-pong through log_put instead. stage_wait is the worker's
    // first statement; root posts once after the last spawn and each worker re-posts.
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

    // The arena's allocation granule, or 0 where it cannot be established.
    // Memoised: on a bump arena kos_ram_alloc never frees, and on a 16 KiB part the arena
    // must stay whole for mem_self_grant to reach the region-descriptor ceiling.
    size_t g_granule = 0;

    size_t discover_granule()
    {
        if (g_granule != 0)
        {
            return g_granule;
        }
#if KICKOS_HAVE_ASPACE
        // ASKED, NOT MEASURED. Under translation kos_ram_alloc reserves frames out of a
        // first-fit bitmap, so the distance between two consecutive results is whatever the
        // holes left by earlier frees make it, and it can be zero or negative. Allocation
        // ORDER IS NOT PUBLIC API on this backend and no arm may derive geometry from it.
        // The power-of-two test rejects an error return, every granule being one.
        uint64_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        if (g == 0 or (g & (g - 1u)) != 0)
        {
            return 0;
        }
        g_granule = static_cast<size_t>(g);
        return g_granule;
#else
        // A bump arena: consecutive results ARE a stride, and there is no probe syscall on a
        // board that builds no selftest kernel half.
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
#endif
    }

    // Nothing waits on this probe, so any subset of a probe batch still drains.
    void pool_probe_worker(void*)
    {
        kos_sem_post(CH_DONE);
    }

    // Can this board host `n` workers CONCURRENTLY, right now? Slots held by service-list
    // drivers and arena room for each stack bound this as much as KICKOS_MAX_THREADS does.
    // Call immediately before the real spawns; when wait_n returns every probe slot is
    // EXITED and every probe stack is back on the free list.
    bool pool_can_host(int n)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        int got = 0;
        for (int i = 0; i < n; i++)
        {
            if (not kos::thread::create_caps(pool_probe_worker, nullptr, "probe", 10, caps, 1).valid())
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
    // NOT a delivery check: kos_kconsole_write returns `len` even when console_emit discards
    // every byte (kernel/init/console.cc, USER_OWNED). Delivery is asserted in cap_index0's
    // post-publish arm and the harness's route probe.
    void t_svc()
    {
        char const* s = "# [svc] kconsole_write arg/return roundtrip (not a delivery check)\n";
        size_t const n = strlen(s);
        TAP_CHECK(kos_kconsole_write(s, n) == static_cast<int32_t>(n));
        TAP_CHECK(kos_kconsole_write(s, 0) == 0); // a len-0 write is a legitimate 0 (sys.h)
        // The prefix must itself be a whole line or the TAP stream is malformed.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto a = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10,
                                          caps, 2);
        auto b = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10,
                                          caps, 2);
        TAP_CHECK(a.valid() and b.valid()); // spawn failure would hang the join below
        wait_n(2);
        TAP_CHECK(log_eq("AB"));
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
        kos_sem_post(CH_AUX); // g_go
        log_put('L');
        kos_sem_post(CH_DONE);
    }
    void t_preempt()
    {
        log_reset();
        kos_sem_create(0, &g_go);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_go, CH_FULL}};
        auto hi = kos::thread::create_caps(preempt_high, nullptr, "high", 20, caps, 3);
        auto lo = kos::thread::create_caps(preempt_low, nullptr, "low", 8, caps, 3);
        TAP_CHECK(hi.valid() and lo.valid()); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_go); // reclaim: the suite must be pool-honest (runs on MAX_SEMAPHORES=4)
        TAP_CHECK(log_eq("lHL"));
    }

    // --- Core clock read syscall -----------------------------------------------
    void t_cpu_clock_hz()
    {
        uint32_t hz = kos_cpu_clock_hz();
        TAP_CHECK(hz == kos_cpu_clock_hz());
        // 0 == the backend has no silicon core clock (host sim); a real core
        // reports a plausible rate (>= 1 MHz, below every board's post-init clock).
        TAP_CHECK(hz == 0u or hz >= 1000000u);
    }

    void t_periph_clock_hz()
    {
        // A base no backend models returns 0 on EVERY target: the dispatch path and the
        // fallback plumbing reach the arch seam.
        uint32_t const bogus = kos_periph_clock_hz(0xDEAD0000u);
        TAP_CHECK(bogus == 0u);
        TAP_CHECK(bogus == kos_periph_clock_hz(0xDEAD0000u));
    }

    // An out-of-range port/pin is REJECTED on every target: -KOS_EINVAL where a chip owns
    // its PORT/IOCR block, -KOS_ENOSYS on the declining-fallback targets (host sim). Both
    // rejects return BEFORE any hardware write.
    // The -KOS_EPERM exclusion is load-bearing: the AUTH_PINMUX gate runs BEFORE the range
    // check, so a bare `rc < 0` would pass just as happily on a root that had lost the bit.
    void t_pinmux_set()
    {
        int32_t const bad_port = kos_pinmux_set(99u, 0u, 0x10u);
        int32_t const bad_pin = kos_pinmux_set(0u, 99u, 0x10u);
        TAP_CHECK(bad_port < 0 and bad_port != -KOS_EPERM);
        TAP_CHECK(bad_pin < 0 and bad_pin != -KOS_EPERM);
    }

    // kos_cpu_clock_set is gated on AUTH_PSTATE: the gate returns the sentinel 0 ("cannot
    // change") to a caller that does not hold it, with NO retune. This MUST run from a
    // spawned child holding no authority: from root, which holds every bit, it would really
    // retune on a chip with a real backend (XMC/K64F) and leave the core clock moved for
    // the rest of the suite. The accepting arm is covered by the clockretune harness; see
    // docs/design-m3-clock-select.md sec 6.
    uint32_t g_clkset_low = 1;
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
        kos_cap_grant caps[] = {{g_clkset_done, CH_FULL}};
        auto w = kos::thread::create_caps(clkset_unpriv_worker, nullptr, "clkset", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_clkset_done);
            return;
        }
        kos_sem_wait(g_clkset_done);
        kos_sem_destroy(g_clkset_done);
        TAP_CHECK(g_clkset_low == 0u);
        TAP_CHECK(g_clkset_mid == 0u);
        TAP_CHECK(g_clkset_max == 0u);
        TAP_CHECK(kos_cpu_clock_hz() == before);
        TAP_CHECK(kos_clock_now() >= t0);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // kos_irq_inject is a KICKOS_ENABLE_SELFTEST-only syscall; without the flag it is a
    // kernel no-op and these arms deadlock on a handler that never fires. The definitions
    // must stay gated together with their registrations in main.
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
        kos_irq_inject(IRQ_CTX_LINE);
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
        auto w = kos::thread::create_caps(irq_waiter, nullptr, "irqW", 15, wcaps, 3);
        auto inj = kos::thread::create_caps(irq_injector, nullptr, "irqI", 8, icaps, 2);
        TAP_CHECK(w.valid() and inj.valid()); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_irq); // reclaim (the line stays bound to a stale handle -> fails safe)
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
        // peer burn a whole slice alone, and the interleave then measures spawn latency.
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
        // The quantum must be resolvable by the monotonic clock or the slice cannot preempt
        // mid-burn. Never hardcode a fine clock: the QEMU semihosting clock is coarse. The
        // probe must SPIN; a WFI would not advance the clock.
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
        g_rr_burn_ns = quantum * 2;

        log_reset();
        kos_sem_create(0, &g_gate);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                {g_gate, CH_FULL}};
        // Must stay UNPRIVILEGED: that is what exercises the region reload per slice.
        auto a = kos::thread::create_caps(rr_worker, reinterpret_cast<void*>('A'), "rrA", 10,
                                          caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                          /*privileged=*/false);
        auto b = kos::thread::create_caps(rr_worker, reinterpret_cast<void*>('B'), "rrB", 10,
                                          caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                          /*privileged=*/false);
        TAP_CHECK(g_gate != KOS_CAP_NONE and a.valid() and b.valid()); // spawn failure would hang the join below
        wait_n(2);
        stage_release();
        wait_n(2);
        kos_handle_close(g_gate);
        // The diag separates "A ran to completion" from "they alternated but B started late",
        // which the predicates below score the same.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto l = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL",
                                          10, caps, 2);
        auto s = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS",
                                          10, caps, 2);
        TAP_CHECK(l.valid() and s.valid()); // spawn failure would hang the join below
        wait_n(2);
        TAP_CHECK(log_eq("SL"));
    }

    // --- Two equal-priority threads blocking on one semaphore ------------------
    // The blocker must detach from the ready list before parking on the wait queue (they
    // share one link node), or the second waiter is orphaned and never wakes.
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
        auto a = kos::thread::create_caps(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10,
                                          caps, 3);
        auto b = kos::thread::create_caps(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10,
                                          caps, 3);
        // A dropped spawn leaves no worker and main hangs in wait_n.
        TAP_CHECK(a.valid() and b.valid());
        kos_sleep_ns(5000000ull); // let both block on g_multi
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        wait_n(2);
        kos_sem_destroy(g_multi); // reclaim
        TAP_CHECK(count('A') == 1 and count('B') == 1);
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
    // WORD 0 IS THE DEVICE, WORDS 1..3 ARE WHAT THE DRIVER SAW. The driver carries the page
    // as its domain grant, so it is a task and an address space of its own and an app global
    // it wrote would be its own copy of one; the block root handed it is what carries the
    // reading back, at the address root named (docs/design-m6-mmu.md F10).
    constexpr int IRQ_LINE = KICKOS_SELFTEST_IRQ_BASE + 1;

    void irq_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        volatile int* const dev = static_cast<volatile int*>(g_mmio);
        kos_sem_post(CH_READY); // g_irq_ready: holds the line cap + about to park
        for (int i = 0; i < 3; i++)
        {
            irq.wait();
            dev[1 + i] = dev[0];
            irq.ack();
            kos_sem_post(CH_DONE); // g_irqdrv_done
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
            tap::skip("4 KiB MMIO-page alloc failed, board too small");
            return;
        }
        // Without this grant the writes below fault: root does not reach its own arena
        // allocations.
        TAP_CHECK(kos_mem_self_grant(g_mmio, 4096, 0) == 0);
        for (int i = 0; i < 4; i++)
        {
            static_cast<volatile int*>(g_mmio)[i] = 0;
        }
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
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(irq_driver, nullptr, "irqdrv", 15, caps, 3,
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
            *static_cast<volatile int*>(g_mmio) = 0x100 + i;
            kos_irq_inject(IRQ_LINE);
            kos_sem_wait(g_irqdrv_done);
        }
        kos_sem_destroy(g_irqdrv_done); // reclaim
        kos_sem_destroy(g_irq_ready);
        volatile int const* const dev = static_cast<volatile int*>(g_mmio);
        TAP_CHECK(dev[1] == 0x101 and dev[2] == 0x102 and dev[3] == 0x103);
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
            irq.ack();
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
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(mask_driver, nullptr, "maskdrv", 1, caps, 3); // below root
        TAP_CHECK(drv.valid());        // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready); // driver holds the line cap, about to wait
        kos_sem_destroy(g_irq_ready);
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        wait_n(2);
        // Bounded settle: a spurious third wake would bump serviced past 2 while the
        // driver is parked in its third wait.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_mask_serviced == 2);
        // Release the parked third wait with a fresh event so the driver exits + joins.
        kos_irq_inject(MASK_LINE);
        wait_n(1);
        TAP_CHECK(g_mask_serviced == 3);
    }

    // --- An EDGE driver can retire a latch it knows is stale -------------------
    // The controller is a reserved block no grant reaches, so kos_irq_discard is a driver's
    // ONLY way to drop a pending it knows is stale: ONE service where coalescing gives two.
    int g_disc_serviced = 0;
    // The one arm that RETIRES a pending: its line must have no source that can re-assert
    // underneath the ICPR write. RP2040 IRQ15 is SIO_IRQ_PROC0, which re-asserts from the
    // core-local FIFO level with no enable bit, and the retired latch redelivers. It also
    // needs a line of its own: the ownership arm leaves its line bound to a stale handle, so
    // sharing that one would make this arm depend on registration order.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int DISCARD_LINE = KICKOS_IRQ_SOFT_ONLY_BASE;
#else
    constexpr int DISCARD_LINE = KICKOS_SELFTEST_IRQ_BASE + 9;
#endif

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
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(discard_driver, nullptr, "discirq", 1, caps, 3);
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);
        // #1 delivers + masks; #2 and #3 coalesce into one latch on the masked line.
        kos_irq_inject(DISCARD_LINE);
        kos_irq_inject(DISCARD_LINE);
        kos_irq_inject(DISCARD_LINE);
        wait_n(1);
        // Bounded settle: without the discard the coalesced latch redelivers here and the
        // driver reaches its second service.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_disc_serviced == 1);
        // Liveness: discard must retire the latch without wedging the line.
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
            irq.wait(); // no ack: the next wait re-arms the line
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
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(autorearm_driver, nullptr, "autoirq", 15, caps, 3);
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);
        for (int i = 0; i < 3; i++)
        {
            kos_irq_inject(AUTO_REARM_LINE);
            wait_n(1);
        }
        TAP_CHECK(g_autorearm_seen == 3);
    }

    // --- No phantom wake in the ack;compute;wait shape -------------------------
    // After an explicit ack re-arms the line, exactly ONE injected event must yield exactly
    // ONE wait-return: the second wait BLOCKS. Setting needs_rearm in the ISR instead of on
    // wait-return unmasks early and phantom-posts. Driver MUST run below root so root
    // sequences each step, and every inject below must target an ARMED line.
    int g_phantom_seen = 0;
    constexpr int PHANTOM_LINE = KICKOS_SELFTEST_IRQ_BASE + 4;

    void phantom_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        kos_sem_post(CH_READY); // g_irq_ready
        irq.wait();
        irq.ack();
        kos_sem_post(CH_DONE); // acked; root injects the one mid-compute event
        irq.wait();
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
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(phantom_driver, nullptr, "phantirq", 1, caps, 3); // below root
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);

        kos_irq_inject(PHANTOM_LINE);
        wait_n(1);

        kos_irq_inject(PHANTOM_LINE); // the one mid-compute event, on the armed line
        wait_n(1);
        TAP_CHECK(g_phantom_seen == 1);

        // The driver is now parked in its third wait. It is lower priority, so
        // sleeping yields the CPU to it: a phantom wake would bump seen here.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_phantom_seen == 1);

        // Wait is live (blocked, not lost) and the line re-armed itself.
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
        TAP_CHECK(kos_sem_destroy(h) == 0);
        TAP_CHECK(kos_sem_destroy(h) == -KOS_EBADF);
        kos_cap_t h2 = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &h2) == 0 and h2 != h); // reused slot carries a fresh generation
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        // Malformed caps must fail with the SPECIFIC code -KOS_EBADF, not any negative.
        // wait/post share the same cap_resolve chokepoint.
        TAP_CHECK(kos_handle_close(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -KOS_EBADF);
        // The count is bounded at both ends: birth outside [0, KOS_SEM_COUNT_MAX] is
        // refused, and a post at the ceiling with no waiter is refused, not overflowed.
        kos_cap_t bad = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(-1, &bad) == -KOS_EINVAL and bad == KOS_CAP_NONE);
        kos_cap_t hmax = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(KOS_SEM_COUNT_MAX, &hmax) == 0
                  and kos_sem_post(hmax) == -KOS_EOVERFLOW
                  and kos_handle_close(hmax) == 0);
    }

    // --- Refcounted close of a DELEGATED sem: object survives while a co-holder is
    // parked; the last close frees it.
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
        kos_sem_post(CH_READY);
        kos_sem_post(CH_DONE);
    }
    void t_sem_destroy_busy()
    {
        kos_sem_create(0, &g_dsem);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_dsem, CH_FULL}};
        auto w = kos::thread::create_caps(destroy_waiter, nullptr, "dwaiter", 15, caps, 2);
        auto p = kos::thread::create_caps(destroy_poster, nullptr, "dposter", 15, caps, 2);
        TAP_CHECK(w.valid() and p.valid()); // spawn failure would hang wait_n(2) below
        kos_sleep_ns(2000000ull);     // let the waiter park on g_dsem; refs = main+waiter+poster = 3
        // Close MAIN's cap WHILE the waiter is parked and the poster has not yet posted:
        // refs 3->2, so the object MUST survive. A freed object or wait queue would leave
        // the poster's later post unable to wake the waiter, and wait_n(2) would hang.
        TAP_CHECK(kos_handle_close(g_dsem) == 0);
        wait_n(2);
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
    // The kos_yield() inside the critical section is load-bearing: without serialization
    // the peer reads the stale value and updates are lost, so conservation is the only pass.
    constexpr int MTX_ITERS = 20;
    int g_mtx_shared = 0;
    // A mutex cap carries CAP_TRANSFER only, so it must be delegated with a TRANSFER-only
    // mask: a CH_FULL mask is not a subset and delegation would reject it.
    constexpr uint8_t CH_MTX = KOS_CAP_TRANSFER;
    void mtx_basic_worker(void*) // caps: done@1, mutex@2
    {
        for (int i = 0; i < MTX_ITERS; i++)
        {
            kos_mutex_lock(2);      // the delegated mutex cap
            int tmp = g_mtx_shared;
            kos_yield();
            g_mtx_shared = tmp + 1;
            kos_mutex_unlock(2);
        }
        kos_sem_post(CH_DONE);
    }
    void t_mutex_basic()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        g_mtx_shared = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        auto a = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbA", 10, caps, 2);
        auto b = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbB", 10, caps, 2);
        auto c = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbC", 10, caps, 2);
        if (not a.valid() or not b.valid() or not c.valid())
        {
            // The partial batch MUST be drained: they post the shared g_done, and a stale
            // post desyncs a later wait_n. Close the mutex too, or the leak cascades through
            // the cap table.
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
        TAP_CHECK(g_mtx_shared == 3 * MTX_ITERS);
    }

    // The PI choreography below holds only if the lock/block/boost chain forms within the
    // slack between scheduled wakes, so the unit must dominate a reschedule round-trip, NOT
    // merely the clock granule. On armv6m (software 64-bit divides in the tickless math)
    // that round-trip is ~10-30 ms, far above a 1 ms unit.
    uint64_t mtx_time_unit()
    {
        // A unit below a few granules is unmeasurable, so the granule is a lower bound.
        uint64_t g0 = kos_clock_now();
        uint64_t g1 = g0;
        while (g1 == g0) { g1 = kos_clock_now(); }
        uint64_t g2 = g1;
        while (g2 == g1) { g2 = kos_clock_now(); }
        uint64_t granule = g2 - g1;

        // Per-sleep OVERHEAD above a small real sleep: the jitter a 1-unit gap must out-scale.
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
            unit = 1000000ull; // 1 ms floor
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
    // boosting low to 20; med(12) wakes next and must NOT preempt the boosted low. Both
    // orderings ('u' before 'm', 'm' before 'z') invert if the boost or the revert is gone.
    uint64_t g_mtx_unit = 1000000ull;
    void pi_low(void*) // caps: done@1, lock@2, mutex@3, gate@4
    {
        stage_wait(4);
        kos_mutex_lock(3);
        log_put('l');
        mtx_spin(g_mtx_unit * 4); // hold across high's and med's wake instants
        log_put('u');
        kos_mutex_unlock(3);
        log_put('z');        // reached only after med (12) has run -> proves revert
        kos_sem_post(CH_DONE);
    }
    void pi_high(void*) // caps: done@1, lock@2, mutex@3, gate@4
    {
        stage_wait(4);
        kos_sleep_ns(g_mtx_unit * 1);
        log_put('h');
        kos_mutex_lock(3);
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
        auto lo = kos::thread::create_caps(pi_low, nullptr, "piLo", 8, lcaps, 4);
        auto hi = kos::thread::create_caps(pi_high, nullptr, "piHi", 20, lcaps, 4);
        auto md = kos::thread::create_caps(pi_med, nullptr, "piMd", 12, mcaps, 3);
        stage_release();
        if (not lo.valid() or not hi.valid() or not md.valid())
        {
            // The partial batch MUST be drained (they post the shared g_done) and the mutex
            // closed, or a later wait_n desyncs.
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
        auto c = kos::thread::create_caps(ch_c, nullptr, "chC", 5, ccaps, 6);
        auto b = kos::thread::create_caps(ch_b, nullptr, "chB", 10, bcaps, 5);
        auto a = kos::thread::create_caps(ch_a, nullptr, "chA", 20, acaps, 4);
        auto d = kos::thread::create_caps(ch_d, nullptr, "chD", 15, dcaps, 3);
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
    // The owner must exit WHILE still holding: that is what makes cap_teardown force-unlock
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
        g_od_result = kos_mutex_lock(2);
        // -KOS_EOWNERDEAD is a HELD acquire: unlock it too, or the robust mutex is stranded.
        // A plain `>= 0` test would skip this, since owner-died is a NEGATIVE code.
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
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}};
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        auto ow = kos::thread::create_caps(od_owner, nullptr, "odOwn", 8, ocaps, 2);
        auto wt = kos::thread::create_caps(od_waiter, nullptr, "odWt", 12, wcaps, 2);
        TAP_CHECK(ow.valid() and wt.valid());
        kos_sem_wait(holds); // owner acquired the mutex (then sleeps, still holding)
        wait_n(1);           // only the waiter posts done (owner exited)
        TAP_CHECK(g_od_result == -KOS_EOWNERDEAD);
        TAP_CHECK(kos_handle_close(m) == 0);
        kos_sem_destroy(holds);
    }

    // --- Deadlock refused -KOS_EDEADLK (H6): self-lock + a two-mutex wait cycle -
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
        g_cyc_rb = kos_mutex_lock(3); // M1: closes the cycle -> refused
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
        TAP_CHECK(kos_mutex_lock(self) == -KOS_EDEADLK);
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
        auto a = kos::thread::create_caps(cyc_a, nullptr, "cycA", 10, acaps, 5);
        auto b = kos::thread::create_caps(cyc_b, nullptr, "cycB", 10, bcaps, 5);
        TAP_CHECK(a.valid() and b.valid());
        kos_sem_wait(have1); // A owns M1
        kos_sem_wait(have2); // B owns M2
        kos_sem_post(goA);   // A tries M2 -> blocks (B owns it)
        kos_sem_post(goB);   // B tries M1 -> would cycle -> -KOS_EDEADLK, not parked
        wait_n(2);
        TAP_CHECK(g_cyc_rb == -KOS_EDEADLK);
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
        TAP_CHECK(kos_handle_close(m) == -KOS_EBUSY);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
    }

    // --- Multiple held mutexes: revert is recompute, NOT restore-to-base (H3) ---
    // B (base 6) holds M1 and M2; H (20) waits on M1, boosting B to 20; D (12) competes. B
    // unlocks M2 while H still waits on M1, so recompute keeps B at 20 and H runs before D.
    // A restore-to-base bug drops B to 6 at the M2 unlock and D runs first.
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
        auto b = kos::thread::create_caps(mh_b, nullptr, "mhB", 6, bcaps, 4);
        auto h = kos::thread::create_caps(mh_h, nullptr, "mhH", 20, hcaps, 3);
        auto d = kos::thread::create_caps(mh_d, nullptr, "mhD", 12, dcaps, 2);
        if (not b.valid() or not h.valid() or not d.valid())
        {
            // The partial batch MUST be drained (they post the shared g_done) and both
            // mutexes closed, or a later wait_n desyncs.
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
        g_nonowner_rc = kos_mutex_unlock(2);
        kos_sem_post(CH_DONE);
    }
    void t_mutex_unlock_errors()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == -KOS_EPERM); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        auto w = kos::thread::create_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_nonowner_rc == -KOS_EPERM);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
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
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}};
        auto ow = kos::thread::create_caps(od_solo_owner, nullptr, "odSolo", 8, ocaps, 2);
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};  // refs -> 2
        auto w = kos::thread::create_caps(deleg_closer, nullptr, "delcl", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        // Child closed its cap (and exited): the object must survive on main's cap.
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        // Pool honesty: create/close well past the pool must not exhaust.
        for (int i = 0; i < 40; i++)
        {
            kos_cap_t x = KOS_CAP_NONE;
            TAP_CHECK(kos_mutex_create(&x) == 0 and kos_handle_close(x) == 0);
        }
    }

    // --- The userspace SPSC byte ring ------------------------------------------
    // Buffer and ring MUST stay on the stack: a static here comes out of the tiny boards'
    // user arena and pushes a later arena probe into a skip.
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
        TAP_CHECK(kos_byte_ring_push(&r, src, 1) == 0);

        unsigned char out[8] = {0};
        TAP_CHECK(kos_byte_ring_pop(&r, out, 2) == 2);
        TAP_CHECK(out[0] == 'a' and out[1] == 'b');
        // Wrap: the two pushed below straddle the end of the buffer, so a mask bug shows
        // up as wrong ORDER here rather than as a bad count.
        TAP_CHECK(kos_byte_ring_push(&r, src, 2) == 2);
        unsigned char one = 0;
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 1);
        TAP_CHECK(one == 'c');
        TAP_CHECK(kos_byte_ring_pop(&r, out, sizeof(out)) == 6);
        TAP_CHECK(out[0] == 'd' and out[1] == 'a' and out[2] == 'b' and out[3] == 'c');
        TAP_CHECK(out[4] == 'a' and out[5] == 'b');
        TAP_CHECK(kos_byte_ring_used(&r) == 0);
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 0);

        // A non-power-of-two size is a programming error and is REFUSED rather than
        // masked wrong: the ring reports empty-and-full instead of corrupting memory.
        struct kos_byte_ring bad;
        kos_byte_ring_init(&bad, buf, 6);
        TAP_CHECK(kos_byte_ring_space(&bad) == 0);
        TAP_CHECK(kos_byte_ring_push(&bad, src, 1) == 0);
    }

    // --- Per-grant destination indices: the refusals ---------------------------
    // A bad placement list must be REFUSED before anything is built.
    // Both spawns below must fail, so this body is deliberately never reached.
    void capdest_never_runs(void*) { kos_exit(0); }

    // Posts the completion sem from a NON-default index. If placement were ignored the post
    // would fail and root would never be released, so the failure surfaces as a TRUNCATED
    // run rather than a `not ok`. Do not add a report channel: two more file-scope words
    // starve microbit's arena.
    void capdest_probe(void*) { kos_sem_post(CH_IRQ); }

    void t_cap_dest()
    {
        kos_cap_t sem = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &sem) == 0);
        kos_cap_grant caps[] = {{sem, CH_FULL}, {sem, CH_FULL}};

        // Two grants naming one slot: the second install would overwrite the first and
        // leak its reference, so the list is refused whole.
        uint16_t const collide[] = {CH_DONE, CH_DONE};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd1", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           collide)
                      .error() == -KOS_EINVAL);

        // A destination past the child's table. cap.h caps KICKOS_MAX_HANDLES at
        // KCAP_RESERVED_INDEX, so index 65535 is out of range on every board.
        uint16_t const far_off[] = {CH_DONE, 65535};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd2", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           far_off)
                      .error() == -KOS_EINVAL);

        // A collision with a DEFAULTED entry counts too: entry 0 defaults to index 1 and
        // entry 1 names it explicitly.
        uint16_t const vs_default[] = {0, CH_DONE};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd3", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           vs_default)
                      .error() == -KOS_EINVAL);

        // The POSITIVE half: delegate the completion sem at index 3 with nothing at 1 or 2.
        // Ignoring the destination puts it at 1 and the worker's post never lands.
        kos_cap_grant one[] = {{g_done, CH_FULL}};
        uint16_t const at3[] = {CH_IRQ};
        auto const w = kos::thread::create_caps(capdest_probe, nullptr, "cdp", 15, one, 1,
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
    // call site is #if KICKOS_CONSOLE_CRLF, so otherwise the expansion ships with coverage
    // on no board at all. Keep it pure stack: microbit's arena is 16 KiB and a `static`
    // comes straight out of it.
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

        // The rule does NOT look back: an input that already carries "\r\n" becomes
        // "\r\r\n". A doubled CR is a no-op on the wire, and this matches kconsole_write.
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

        // The write policy KOS_UART_SET_MODE carries, decided by one pure function for every
        // transport.
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

        // A non-blocking write REPORTS its short accept: the service arm turns the shortfall
        // into stats.tx_dropped, the only channel an unframed writer can read.
        unsigned char rbuf[8];
        struct kos_byte_ring ring;
        kos_byte_ring_init(&ring, rbuf, sizeof(rbuf));
        struct kos_uart_stats st = {};
        unsigned char twelve[12];
        memset(twelve, 'z', sizeof(twelve));
        // Eight bytes of room for twelve offered: a short accept, not a refusal and not a
        // wait.
        uint32_t const nb = kickos::console::write_console(&ring, &st, twelve, 12,
                                                          KOS_UART_F_NONBLOCK);
        TAP_CHECK(nb < 12);
        TAP_CHECK(12u - nb > 0u); // the shortfall the service arm charges to tx_dropped
        uint32_t const cooked = kos_counter_load(&st.tx_bytes);
        TAP_CHECK(cooked > 0u and cooked <= sizeof(rbuf));
        // The return is INPUT bytes and the counter is COOKED bytes, so these differ under
        // KICKOS_CONSOLE_CRLF. Asserting equality here would pass on the sim, the ONLY
        // crlf=0 tree (the root CMakeLists sets crlf=1 for every arch that is not sim),
        // and fail on every other board.
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
        TAP_CHECK(kos_irq_attach(LINE, sem) == 0);
        TAP_CHECK(kos_irq_attach(LINE, sem) == -KOS_EBUSY);
        // Tier-1 cannot steal it either. Root holds KOS_AUTH_IRQ, so this is the
        // one-owner-per-line refusal and not the authority gate.
        kos_cap_t stolen = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(LINE, KOS_IRQ_EDGE, &stolen) == -KOS_EBUSY
                  and stolen == KOS_CAP_NONE);
        kos_sem_destroy(sem); // reclaim (the line stays bound to a stale handle -> fails safe)
    }

    // --- The tier-1 mint is gated on KOS_AUTH_IRQ ------------------------------
    // The refusal MUST be witnessed from a worker, not from root: the suite declares
    // KOS_AUTH_IRQ, so a root-side claim tests the GRANT and can never see the refusal.
    // Keep this to ONE new static: on a 16 KiB part .bss added here shrinks the arena for
    // every later arm.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // ABOVE root, so it runs to completion including its exit before root is scheduled
        // again: its slot is then EXITED and reusable by the next arm's worker.
        auto w = kos::thread::create_caps(claimgate_worker, nullptr, "claimgate", 15, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // EPERM, not EBUSY: the line is free, so only the authority gate can refuse it.
        TAP_CHECK(g_claimgate_rc == -KOS_EPERM);
        // The refusal left NOTHING behind: root can still claim that same line.
        kos_cap_t owned = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &owned) == 0);
        TAP_CHECK(kos_handle_close(owned) == 0);
    }

    // --- A line comes back when its holder dies --------------------------------
    // A dying thread's line cap is dropped and the binding slot freed, so the SAME line is
    // claimable again. Without that release the line returns -KOS_EBUSY forever.
    constexpr int RECLAIM_LINE = KICKOS_SELFTEST_IRQ_BASE + 8;

    // Do NOT rewrite this as wait-then-inject: a claim leaves the line masked and spawn
    // does not preempt, so root's inject lands masked, the worker's own first arm discards
    // the latch, and the arm deadlocks instead of testing. The ack reaches the ARMED state
    // with no event needed.
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
        auto w = kos::thread::create_caps(reclaim_worker, nullptr, "reclaim", 15, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                          /*authority=*/0, dest);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_handle_close(first);
            return;
        }
        // Root drops its copy BEFORE the worker dies, so the worker's exit is what takes the
        // refcount to zero. Closing after would not prove that DEATH releases the line.
        TAP_CHECK(kos_handle_close(first) == 0);
        wait_n(1);
        // The worker is higher priority, so it has exited by the time root runs again.
        kos_cap_t second = KOS_CAP_NONE;
        // -KOS_EBUSY here means death did not release the line
        TAP_CHECK(kos_irq_claim(RECLAIM_LINE, KOS_IRQ_EDGE, &second) == 0);
        TAP_CHECK(kos_handle_close(second) == 0);
    }

    // --- Spurious IRQ: an unbound line is masked + counted, never dropped -------
    void t_irq_spurious()
    {
        constexpr int FREE_LINE = KICKOS_SELFTEST_IRQ_BASE + 3; // no driver bound to this line
        // Enable the line so the injected raise reaches the default handler: ARM NVIC and RX
        // are masked by default, sim/riscv are not.
        kos_irq_unmask(FREE_LINE);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(FREE_LINE);   // default handler runs: mask + bump counter
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        // The default handler masked the line, so a second raise LATCHES rather than
        // delivering: the counter must NOT advance until the line is unmasked again.
        kos_irq_inject(FREE_LINE);
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
    }

    // --- First-arm discards pre-claim garbage ----------------------------------
    // A raise that lands before a driver owns the line is latched, and the FIRST ARM must
    // DISCARD that stale latch (arch_irq_clear_pending) or the first irq.wait() phantom-
    // wakes on garbage. This is the ONE tier-1 arm where root must NOT pre-arm the line:
    // pre-arming moves the discard into root and stops testing the driver's own first arm.
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
        // Pre-registration garbage: unmask so the default handler runs (mask + count) on the
        // first raise; the second raise then latches on the now-masked line.
        kos_irq_unmask(STALE_LINE);
        uint32_t before = kos_irq_spurious_count();
        kos_irq_inject(STALE_LINE);
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
        kos_irq_inject(STALE_LINE);
        // Claim but deliberately do NOT arm: the latch must still be there for the
        // driver's own first wait to discard.
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(STALE_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(stale_driver, nullptr, "staleirq", 1, caps, 3); // below root
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
    // NON-ENFORCING builds ONLY: KOS_STACK_DEFINE aligns to 16 bytes without an MPU. Under
    // MPU it aligns to a whole region and the static would not fit a fixed small .appdata
    // window (C6 = 4K); the dynamic stack above covers the MPU case. A TRANSLATING backend
    // has no descriptors either, but it does confine an unprivileged stack to the arena,
    // and a static lives outside it.
#if !KICKOS_MEMORY_ENFORCED
    // The worker runs the deepest kernel dispatch on THIS stack, so the size must clear the
    // per-arch KICKOS_MIN_STACK_SIZE floor. Under KICKOS_TLS a seated block must ALSO match the
    // stride exactly, and a caller stack that does not is refused rather than left unseated.
#if defined(KICKOS_TLS) && KICKOS_TLS
    KOS_STACK_DEFINE(g_cstk_static, KICKOS_TLS_STRIDE);
#else
    KOS_STACK_DEFINE(g_cstk_static, KICKOS_MIN_STACK_SIZE);
#endif
#endif
    void t_caller_stack()
    {
        // Reject a non-null, tiny + misaligned caller stack: -KOS_EINVAL, not run or corrupt.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8).error()
                  == -KOS_EINVAL);
        // Accept a properly-sized, aligned caller-owned stack. When the arena cannot spare
        // one the reject case above has already run, so the arm stays `ok` as a partial.
        constexpr uint32_t STK = 2048;
        void* raw = kos_ram_alloc(STK + 16);
        if (raw == nullptr)
        {
            tap::partial("accept half not run (arena cannot spare a stack)");
            return;
        }
        // REACHED BEFORE IT IS HANDED IN. Allocation grants nothing, and where a backend
        // translates that means the block is not even MAPPED: a child started on it would
        // fault on its first push (docs/design-m6-mmu.md F10).
        TAP_CHECK(kos_mem_self_grant(raw, STK + 16, 0) == 0);
        void* stk = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
        // Reject a PROPERLY-ALIGNED but sub-floor caller stack: an aligned 512 B stack
        // passes alignment yet overflows the RISC-V exit dispatch (~624 B). One
        // KICKOS_STACK_ALIGN unit below the floor with an aligned base, so only the size
        // check can reject it.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "undf", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, stk, KICKOS_MIN_STACK_SIZE - 16u,
                                      nullptr, 0, nullptr, 0).error()
                  == -KOS_EINVAL);
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant caps[] = {{g_cstk_sem, CH_FULL}};
        auto const t = kos::thread::create(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                           0, false, nullptr, 0, stk, STK, nullptr, 0, caps, 1);
        TAP_CHECK(t.valid());
        kos_sem_wait(g_cstk_sem);
        kos_sem_destroy(g_cstk_sem);
#if !KICKOS_MEMORY_ENFORCED
        // This buffer is only 16-byte aligned (no MPU), and spawn must still accept and run
        // it: with no region descriptor the natural-alignment check must not apply.
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant scaps[] = {{g_cstk_sem, CH_FULL}};
        auto const ts = kos::thread::create(caller_stack_worker, nullptr, "cstkS", 10, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, g_cstk_static,
                                            static_cast<uint32_t>(sizeof(g_cstk_static)),
                                            nullptr, 0, scaps, 1);
        TAP_CHECK(ts.valid());
        kos_sem_wait(g_cstk_sem);
        kos_sem_destroy(g_cstk_sem);
#endif
    }

    // --- Caller-owned stack outside the arena ------------------------------------
    // A KOS_STACK_DEFINE stack lands in app .bss, which is outside the user-RAM arena, and
    // an unprivileged child's stack is an arena-confined grant: -KOS_EPERM, and no other
    // code, a block failing geometry or the TLS seat answering -KOS_EINVAL. Sized AND
    // aligned to the TLS stride so the seat would admit it and only the arena bound can
    // refuse it.
    // Translating backends only. An enforcing MPU board refuses the same block, but a
    // stride-aligned static does not fit every .appdata window (esp32c6 carves 4K) and
    // stackbase_arena witnesses the bound there.
#if KICKOS_HAVE_ASPACE
#if defined(KICKOS_TLS) && KICKOS_TLS
    alignas(KICKOS_TLS_STRIDE) unsigned char g_cstk_outside[KICKOS_TLS_STRIDE];
#else
    KOS_STACK_DEFINE(g_cstk_outside, KICKOS_MIN_STACK_SIZE);
#endif
    void t_caller_stack_arena()
    {
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "cstkO", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, g_cstk_outside,
                                      static_cast<uint32_t>(sizeof(g_cstk_outside))).error()
                  == -KOS_EPERM);
    }
#endif

    // --- A self-granted range shared by three threads of ONE task --------------------
    // The two workers are plain spawns, so they are threads of ROOT'S task and share the four
    // globals below with it. Each ASKS for the range itself, which is the portable floor: a
    // grant guarantees access to its HOLDER and says nothing about a peer, so a sibling that
    // never asked may be denied on a descriptor board (F9). Where a backend translates the
    // ask costs nothing, the mapping being task-wide and F10's already-mapped short-circuit
    // answering it; sibling VISIBILITY, which only that backend promises, is
    // task_siblings_share.
    volatile int* g_dshared = nullptr;
    kos_cap_t g_dwrote = KOS_CAP_NONE; // writer -> reader handoff (through the handed-over range)
    kos_cap_t g_dread = KOS_CAP_NONE;  // reader -> main handoff
    int g_dreadback = -1;
    constexpr int DOM_SENTINEL = 0x5A5A;
    void dom_writer(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        *g_dshared = DOM_SENTINEL;
        kos_sem_post(CH_DONE);     // g_dwrote
    }
    void dom_reader(void*) // caps: g_dwrote@1 (CH_DONE), g_dread@2 (CH_READY)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        kos_sem_wait(CH_DONE);    // g_dwrote: after the writer stored the sentinel
        g_dreadback = *g_dshared;
        kos_sem_post(CH_READY);   // g_dread
    }
    void t_domain_share()
    {
        // Alloc before the sems so an early return leaks nothing.
        g_dshared = static_cast<volatile int*>(kos_ram_alloc(256));
        if (g_dshared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // Root's own reach, which is what the workers then inherit by being its siblings.
        TAP_CHECK(kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0) == 0);
        *g_dshared = 0;
        g_dreadback = -1;
        kos_sem_create(0, &g_dwrote);
        kos_sem_create(0, &g_dread);
        // Spawn BOTH before either runs (spawn does not preempt).
        kos_cap_grant wcaps[] = {{g_dwrote, CH_FULL}};
        kos_cap_grant rcaps[] = {{g_dwrote, CH_FULL}, {g_dread, CH_FULL}};
        auto w = kos::thread::create_caps(dom_writer, nullptr, "domW", 10, wcaps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY);
        auto r = kos::thread::create_caps(dom_reader, nullptr, "domR", 10, rcaps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY);
        if (not w.valid() or not r.valid())
        {
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
    // The positive grant is HW-only, so this arm pins the two refusals: a window one MPU
    // descriptor cannot cover exactly, and any grant attempted by an UNPRIVILEGED caller.
    // The sim's arch_mpu_region_encodable admits ONE window (its fake register block) and
    // neither of these names it, so both halves still land as a -1 spawn there.
    int g_mmio_unpriv_rc = -2;
    kos_cap_t g_mmio_done = KOS_CAP_NONE;
    void mmio_noop(void*) {}
    void mmio_unpriv_worker(void*)
    {
        // Unprivileged caller: the privilege gate must refuse the MMIO grant.
        g_mmio_unpriv_rc = kos::thread::create(mmio_noop, nullptr, "mmiochild", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0,
                                               reinterpret_cast<void*>(0x1000u), 4096)
                               .error();
        kos_sem_post(CH_DONE); // g_mmio_done
    }
    void t_mmio_grant()
    {
        // Non-encodable window (size 1, unaligned base): rejected -KOS_EINVAL, not rounded.
        // Geometry is checked ahead of the privilege gate, so the code holds in any posture.
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmiobad", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      reinterpret_cast<void*>(0x1001u), 1).error() == -KOS_EINVAL);
        // A non-null base with size 0 is rejected at the boundary (before domain_for).
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmio0", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      reinterpret_cast<void*>(0x2000u), 0).error() == -KOS_EINVAL);
        // A window whose base+size wraps the address space is rejected -KOS_EINVAL (32-bit
        // MCU; on the 64-bit sim the fail-closed encoder rejects it first, either way EINVAL).
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmioW2", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      reinterpret_cast<void*>(0xFFFFFFF0u), 0x20).error() == -KOS_EINVAL);
        kos_sem_create(0, &g_mmio_done);
        g_mmio_unpriv_rc = -2;
        kos_cap_grant caps[] = {{g_mmio_done, CH_FULL}};
        auto w = kos::thread::create_caps(mmio_unpriv_worker, nullptr, "mmioW", 10, caps, 1);
        if (not w.valid())
        {
            // The three encodability cases above already ran, so this stays `ok` and names
            // the dropped half.
            tap::partial("unprivileged half not run (thread pool too small)");
            kos_sem_destroy(g_mmio_done);
            return;
        }
        kos_sem_wait(g_mmio_done);
        kos_sem_destroy(g_mmio_done);
        TAP_CHECK(g_mmio_unpriv_rc == -KOS_EPERM);
    }

    // --- stack_base arena containment (unprivileged self-grant) -----------------
    // The stack_base grant is the ONE unprivileged path that reaches an MPU region. Without
    // an arena bound an unprivileged thread spawns a child with stack_base in peripheral
    // space or kernel SRAM: an R|W window the MMIO gate would refuse. Enforcing backends
    // only: the escalation needs a region descriptor to land in.
#if KICKOS_HAVE_MPU
    int g_stkarena_rc = -2;
    kos_cap_t g_stkarena_done = KOS_CAP_NONE;
    void stkarena_noop(void*) {}
    void stkarena_unpriv_worker(void*)
    {
        // Unprivileged caller; stack_base far above any SRAM arena and naturally aligned
        // (clears the size/align/natural checks) so ONLY the arena bound can reject it.
        g_stkarena_rc = kos::thread::create(stkarena_noop, nullptr, "stkbad", 10,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                            reinterpret_cast<void*>(0xE0000000u), 2048)
                            .error();
        kos_sem_post(CH_DONE); // g_stkarena_done
    }
    void t_stackbase_arena()
    {
        kos_sem_create(0, &g_stkarena_done);
        g_stkarena_rc = -2;
        kos_cap_grant caps[] = {{g_stkarena_done, CH_FULL}};
        auto w = kos::thread::create_caps(stkarena_unpriv_worker, nullptr, "stkW", 10, caps, 1);
        if (not w.valid())
        {
            // The unprivileged child IS this arm, so a missing thread is a whole-arm SKIP,
            // never a partial pass.
            tap::skip("thread pool too small");
            kos_sem_destroy(g_stkarena_done);
            return;
        }
        kos_sem_wait(g_stkarena_done);
        kos_sem_destroy(g_stkarena_done);
        TAP_CHECK(g_stkarena_rc == -KOS_EPERM);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Rule 7 grant predicates: the overlap matrix + RAM/DEV admission ---------
    // Exercises grant_hits_reserved / grant_region_admissible through kos_grant_probe. The
    // reserved-OVERLAP matrix needs a board that declares reserved blocks; the sim reserves
    // nothing, so that half reports PARTIAL there. The bit-band alias-hit case is HW-only.
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
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a, g) == 1);
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_UNPRIVILEGED, a, g) == 1);
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

        // --- Non-cacheable admission is THREE-valued: PROGRAMMED and INHERENT both admit,
        // REFUSED must refuse HERE, since every commit backend drops a region it cannot
        // encode in silence.
        uintptr_t const nc = kos_grant_probe(KOS_GRANT_OP_NOCACHE_SUPPORT, 0, 0);
        TAP_CHECK(nc <= 2); // enum arch_mpu_nocache; a bad op would answer -KOS_EINVAL cast up
        if (raw != nullptr)
        {
            uintptr_t const a = reinterpret_cast<uintptr_t>(raw);
            uintptr_t expect = 1;
            if (nc == 0)
            {
                expect = 0; // ARCH_MPU_NOCACHE_REFUSED
            }
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_NOCACHE, a, g) == expect);
        }
        else
        {
            tap::partial("non-cacheable RAM admission not run (granule alloc failed)");
        }

        // --- End-to-end errno coherence: an unprivileged child whose mem_base lies OUTSIDE
        // the arena is refused with -KOS_EPERM (policy refusal), NOT -KOS_ENOMEM. The code
        // must come from domain_for, not a pre-check at the spawn boundary. 0xE0000000 is
        // 2048-aligned, so ONLY arena containment can reject it.
        auto const mrc = kos::thread::create(grant_noop, nullptr, "membad", 10, KOS_POLICY_FIFO,
                                             0, /*privileged=*/false,
                                             reinterpret_cast<void*>(0xE0000000u), 2048);
        TAP_CHECK(mrc.error() == -KOS_EPERM);

        // --- Reserved-overlap matrix. ---
        // The NON-overlap cases must land in a GAP, so they anchor on scanned edges: the
        // lowest reserved base has nothing flush below it and the highest reserved end
        // nothing at-or-above it, even when blocks are flush (rp2040 TIMER abuts WATCHDOG).
        // Testing block[0]-relative "above" would false-hit on such a board.
        uintptr_t const n = kos_grant_probe(KOS_GRANT_OP_RESERVED_COUNT, 0, 0);
        if (n == 0)
        {
            tap::partial("reserved-overlap matrix not run (board reserves nothing)");
            return;
        }
        uintptr_t const rb = kos_grant_probe(KOS_GRANT_OP_RESERVED_BASE, 0, 0);       // block[0].base
        uintptr_t const rs = kos_grant_probe(KOS_GRANT_OP_RESERVED_SIZE, 0, 0);       // block[0].size
        uintptr_t const rlast = rb + rs - 1u;
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
        // All overlap block[0], so they hit regardless of layout.
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
        // still hits, so the boundary is exact and not merely an empty gap.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, lo_base, 1u) == 1);       // first byte of the lowest block
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end - 1u, 1u) == 1);   // last byte of the highest block
        // Rule 7 core: a reserved block is inadmissible as a DEV grant (privileged too)
        // and as RAM.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, rb, rs) == 0);
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, rb, rs) == 0);
        // End-to-end: a privileged spawn granting the reserved MMIO window is refused.
        auto const rc = kos::thread::create(grant_noop, nullptr, "rsvd", 10, KOS_POLICY_FIFO,
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
    // The holder must stay ALIVE for the whole matrix, and that cannot rest on it not being
    // scheduled: a reschedule between two spawns lets it exit and free the window, turning
    // every refusal below into an admission. It parks on a semaphore instead.
    //
    // The window is DISCOVERED, not hardcoded: reserved blocks and bit-band aliases differ
    // per chip. The sim admits exactly one DEV window, its fake register block at
    // SIM_PVREG_WINDOW (64 KiB), so a WIN-sized search finds nothing there and the case
    // reports PARTIAL. On any other enforcing board an empty search must FAIL.
    constexpr int CH_DEVHOLD = 2; // the holder gate, delegated SECOND (done@1, hold@2)
    void devexcl_hold(void*) // caps: done@1, hold@2
    {
        kos_sem_wait(CH_DEVHOLD); // hold the window until root releases it
        kos_sem_post(CH_DONE);
    }
    void t_dev_window_exclusive()
    {
        constexpr uint32_t WIN = 0x100u; // pow2 >= 32: encodable on PMSAv7/v8 and byte-granular SYSMPU
        // Step 2*WIN so both `base` and its sibling `base + WIN` stay WIN-aligned: PMSA needs
        // natural alignment, so an unaligned base would be refused as unencodable, not held.
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_sem_create(0, &hold) != 0)
        {
            tap::fail("no semaphore for the holder gate; exclusivity cannot be staged");
            return;
        }
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {hold, CH_FULL}};
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
            holder = kos::thread::create(devexcl_hold, nullptr, "devheld", 10, KOS_POLICY_FIFO,
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
            // Positively the sim, so a new arch with no DEV encoder fails loudly here.
#if KICKOS_ARCH_SIM
            // The sim admits exactly ONE DEV window shape (64 KiB), never a WIN-sized one.
            // Assert that premise instead of skipping: the sim gate reads a skip as an arm
            // that stopped running (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            TAP_CHECK(not kos::thread::create(devexcl_hold, nullptr, "devnone", 10,
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
            tap::fail("no DEV-admissible window in [0x40000000, 0x40100000): "
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
                // EPERM/EINVAL here is a boundary failure and not an unrunnable case, so
                // it fails the arm instead of skipping it.
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
        TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devdup", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      reinterpret_cast<void*>(win), WIN, hcaps, 2).error() == -KOS_EBUSY);
        // 2. PARTIAL overlap: the upper half of the held window. Its own base/size are
        //    independently admissible, so only the overlap scan can refuse it.
        TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devpart", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      reinterpret_cast<void*>(win + WIN / 2u), WIN / 2u, hcaps, 2).error()
                  == -KOS_EBUSY);
        // 3. ADJACENT but disjoint (base == held last + 1): ADMITTED. This is the mk64f PIT
        //    CH2 shape: a grant flush against a block, which must not be read as overlapping.
        auto const adj = kos::thread::create(devexcl_hold, nullptr, "devadj", 10, KOS_POLICY_FIFO,
                                             0, false, nullptr, 0, nullptr, 0,
                                             reinterpret_cast<void*>(win + WIN), WIN, hcaps, 2);
        TAP_CHECK(adj.valid());
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
        // The SAME grant refused above must now succeed, so the refusal tracked live holders
        // and not the address. CH_DONE is posted before the holder returns, so retry rather
        // than assume the domain is already released.
        int again = -KOS_EBUSY;
        for (int i = 0; i < 100 and again == -KOS_EBUSY; i++)
        {
            again = kos::thread::create(devexcl_hold, nullptr, "devagain", 10, KOS_POLICY_FIFO,
                                        0, false, nullptr, 0, nullptr, 0,
                                        reinterpret_cast<void*>(win), WIN, hcaps, 2)
                        .error();
            if (again == -KOS_EBUSY)
            {
                kos_sleep_ns(1000000ull); // 1 ms
            }
        }
        TAP_CHECK(again == 0);
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
    // syscall_dispatch runs privileged and bypasses the MPU, so a user pointer it READS (the
    // kconsole_write buffer, a thread name) must lie in memory the UNPRIVILEGED caller could
    // itself reach. A rodata string literal MUST be accepted; a pointer into no granted
    // region (the un-owned guard page) MUST be rejected, never read. All checks run from a
    // spawned UNPRIVILEGED worker, whose granted set is the narrow one the floor is about.
    // The positive half proves only that the floor ACCEPTED the pointer, not that bytes
    // reached a console. It is non-vacuous only when PAIRED with the guard-page negative
    // below.
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
        g_cd_lit_rc = kos_kconsole_write(CD_LIT, strlen(CD_LIT));

        // cd_worker creates its OWN sem (unprivileged create is allowed) and RE-delegates
        // it to a grandchild: nested delegation requires the source cap carry TRANSFER,
        // which sem_create grants. g_cd_kidsem is cd_worker's cap value (its table).
        kos_sem_create(0, &g_cd_kidsem);
        kos_cap_grant kidcaps[] = {{g_cd_kidsem, CH_FULL}};  // grandchild's index 1
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        g_cd_goodspawn = kos::thread::create_caps(cd_kid, &g_cd_goodname_ran, "cdgood", 9,
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
            // Bogus console buffer: rejected, and never read (a wrong-accept would return 8,
            // having read the guard page the caller cannot reach).
            g_cd_bad_rc = kos_kconsole_write(bad, 8);
            // Bogus NAME pointer: the kernel must bound the walk (no fault), drop the
            // name, and still spawn the child.
            g_cd_badname_spawn = kos::thread::create_caps(cd_kid, &g_cd_badname_ran,
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
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    // --- The address-space seam (kos_aspace_probe) ------------------------------
    // The map editor is a KERNEL seam, so each arm asks for a whole scenario rather than
    // for a mapping; the probe answers a number. What every arm is really guarding against
    // is an identity map answering in the editor's place, which passes a careless version
    // of all of them (docs/design-m6-mmu.md section 3.2).
    void t_aspace_seam()
    {
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        TAP_CHECK(g != 0);
        TAP_CHECK((g & (g - 1u)) == 0);
        // All three types honoured, so no grant is admitted and then quietly downgraded.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 0) == 1); // normal
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 1); // non-cacheable
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 2) == 1); // device
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 3) == 0); // no such type
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) != 0);
        tap::diag("aspace: granule %u, %u frames free",
                  static_cast<unsigned>(g),
                  static_cast<unsigned>(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0)));
    }

    // --- What the machine reports, against what the port took from the manuals (F7, S2) ---
    // The arm above re-reads the granule the backend itself defines, so it cannot diverge
    // from it. This one asks the IMPLEMENTATION: the granule it supports, the width of its
    // address-space identifier and the physical range it can address, each compared against
    // the figure this port programs or records. The first target is an emulator, so a
    // divergence is a fact about the machine and never a reason to change the port.
    void t_aspace_model()
    {
        uint64_t const m = kos_aspace_probe(KOS_ASPACE_OP_MODEL, 0);
        unsigned const asid =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_ASID_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        unsigned const pa =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_PA_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        unsigned const grans =
            static_cast<unsigned>((m >> KOS_ASPACE_MODEL_GRAN_SHIFT) & KOS_ASPACE_MODEL_FIELD_MASK);
        tap::diag("aspace model: granules 0x%x, %u ASID bits, %u PA bits, verdict 0x%x",
                  grans, asid, pa, static_cast<unsigned>(m & KOS_ASPACE_MODEL_FIELD_MASK));
        // A machine whose report decodes to nothing would satisfy an equality against
        // another zero, so the widths are read before the verdict is believed. The identifier
        // width is DIAGNOSED and not asserted: the architecture permits it to be hardwired to
        // zero, and a port that tags no translation is a legal port (docs/design-m6-mmu.md F1).
        TAP_CHECK(pa != 0 and grans != 0);
        TAP_CHECK((m & KOS_ASPACE_MODEL_ALL) == KOS_ASPACE_MODEL_ALL);
    }

    void t_aspace_map_cycle()
    {
        // Map, write, read back, unmap, and the page GONE. The fourth is the one a first
        // implementation leaves out, and an editor that never removed the entry stops here.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_ROUNDTRIP, 0) == KOS_ASPACE_TRIP_GONE);
    }

    void t_aspace_translate()
    {
        // Two unequal virtual pages onto ONE frame, the write through either seen through
        // the other and in the frame itself. An identity map hands back two distinct pages
        // and cannot pass this.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_ALIAS, 0) == 1);
    }

    void t_aspace_refusals()
    {
        // Every refusal, as one word: a kernel-half address, a sub-granule base, an empty
        // range, permissions this architecture cannot express, an unknown right, and an
        // unmap of a range not wholly mapped.
        uintptr_t const bits = kos_aspace_probe(KOS_ASPACE_OP_REFUSALS, 0);
        TAP_CHECK(bits == KOS_ASPACE_REFUSE_ALL);
    }

    void t_aspace_span()
    {
        // A run from the last slot of one LAST-LEVEL table, long enough to cross two table
        // boundaries on any geometry the seam admits: the shape a process image has, and the
        // one a miscounted last slot silently truncates. The kernel side derives the length
        // from the granule, no level count being knowable above the seam.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_SPAN, 0) == 1);
    }

    void t_aspace_balance()
    {
        // Create, map, unmap, destroy, four times over: every frame back, tables included.
        // A build whose destroy is a stub passes every arm above while leaking each space.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    void t_aspace_domain_balance()
    {
        // A domain resolved and dropped must return the address space it took, and a free
        // slot must not be reused with its predecessor's space still standing.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_DOMAIN_BALANCE, 0) == 0);
    }

    // --- T8b: THE FORCED FAILURE ------------------------------------------------------
    // Every balance arm above weighs a create that SUCCEEDED. The unwind arms under
    // claim_slot and domain_for are reached only by a create that fails partway, and nothing
    // else in the tree can produce one: the pool is sized for the image, so an arm cannot
    // exhaust it. The kernel refuses one chosen allocation and walks that choice through the
    // whole create, so each arm runs once and alone.
    //
    // THE DEPTH IS READ AND NOT ONLY THE BITS: every property holds vacuously over a sweep
    // that injected nothing, which is what a create path that allocated nothing would give.
    void t_aspace_forced_unwind()
    {
        uintptr_t const r = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND, 0);
        uintptr_t const bits = r & KOS_ASPACE_UNWIND_ALL;
        uintptr_t const depth = r >> KOS_ASPACE_UNWIND_DEPTH_SHIFT;
        tap::diag("forced unwind: bits %u of %u over %u injected allocation(s)",
                  static_cast<unsigned>(bits),
                  static_cast<unsigned>(KOS_ASPACE_UNWIND_ALL),
                  static_cast<unsigned>(depth));
        TAP_CHECK(depth >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        TAP_CHECK(bits == KOS_ASPACE_UNWIND_ALL);
        // THE SAME SWEEP OVER THE GRANT-CARRYING CREATE, which needs a donor range. What it
        // adds is the successful tail: the space that survives the sweep holds a BORROWED
        // mapping of root's block, so its release has to unmap and free nothing, and the
        // REUSABLE bit is what says the pool came back whole after it.
        //
        // IT DOES NOT REACH domain_for's OWN UNWIND ARM, and the depths printed below are
        // what says so: the handoff's map needs no new table on this board, the donor block
        // sitting under a last-level table the image already built, so frame injection has no
        // point inside aspace_handoff and every refusal still lands in claim_slot ahead of it.
        void* const block = kos_ram_alloc(64);
        if (block == nullptr)
        {
            tap::skip("no reservation left to sweep the grant-carrying create with");
            return;
        }
        uintptr_t const h = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND,
                                            reinterpret_cast<uintptr_t>(block));
        uintptr_t const hbits = h & KOS_ASPACE_UNWIND_ALL;
        uintptr_t const hdepth = h >> KOS_ASPACE_UNWIND_DEPTH_SHIFT;
        tap::diag("forced unwind, with a grant: bits %u of %u over %u injected allocation(s)",
                  static_cast<unsigned>(hbits),
                  static_cast<unsigned>(KOS_ASPACE_UNWIND_ALL),
                  static_cast<unsigned>(hdepth));
        TAP_CHECK(hdepth >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        TAP_CHECK(hbits == KOS_ASPACE_UNWIND_ALL);
    }

    // --- T8b: THE CHURN -----------------------------------------------------------------
    // A process created and ended, over and over, with the forced-failure sweep between the
    // rounds so a refused create and a lived-out one share one balance. What it adds over the
    // arms above is the production path: they end a space through the map editor, this one
    // through a task create, a member running to its end, and the creator hold dropped.
    constexpr uint32_t CHURN_JOIN_US = 60000;
    // What one process holds at the least: a root, the two tables its image needs, and one
    // page of the private data copy. The real figure is dozens; this is a floor an arm that
    // created nothing cannot clear.
    constexpr uintptr_t CHURN_MIN_FRAMES = 4;

    void churn_member(void*)
    {
        kos_exit(0);
    }

    // One whole life. `held` is read with the space ALIVE and before the member starts, so
    // it is the space's own cost and races with nothing.
    bool churn_cycle(uintptr_t* held)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        if (held != nullptr)
        {
            *held = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        }
        auto const m = kos::thread::create_caps(churn_member, nullptr, "chrn", 10, nullptr, 0,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                               nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        bool const joined = m.join(CHURN_JOIN_US) == 0;
        // EMPTIED IS NOT DEAD. The member has gone but root still holds the creator's
        // reference, so the space and every frame of it are legitimately still out until this
        // call; a count read before it reports a leak that is not one (T8, on T4's rule).
        bool const reaped = kos_task_kill(t) == 0;
        return joined and reaped;
    }

    void t_aspace_churn()
    {
        // A WARM-UP CYCLE FIRST, for the reason t_stack_is_frames states: the first map into
        // a virtual range allocates intermediate tables that are never pruned, so a first
        // cycle can spend frames a second one does not.
        if (not churn_cycle(nullptr))
        {
            tap::skip("no task or thread slot to churn with");
            return;
        }
        uintptr_t const frames0 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const roots0 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        uintptr_t low = frames0;
        for (int i = 0; i < 4; i++)
        {
            uintptr_t held = frames0;
            TAP_CHECK(churn_cycle(&held));
            if (held < low)
            {
                low = held;
            }
            uintptr_t const u = kos_aspace_probe(KOS_ASPACE_OP_FORCED_UNWIND, 0);
            TAP_CHECK((u & KOS_ASPACE_UNWIND_ALL) == KOS_ASPACE_UNWIND_ALL);
            TAP_CHECK((u >> KOS_ASPACE_UNWIND_DEPTH_SHIFT) >= KOS_ASPACE_UNWIND_MIN_DEPTH);
        }
        uintptr_t const frames1 = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t const roots1 = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        tap::diag("churn: %u frames free, %u with a process live, %u after; roots %u then %u",
                  static_cast<unsigned>(frames0), static_cast<unsigned>(low),
                  static_cast<unsigned>(frames1), static_cast<unsigned>(roots0),
                  static_cast<unsigned>(roots1));
        // THE COUNTS MOVED. An arm that created and destroyed nothing balances trivially, so
        // the drop while a process was alive is what makes the return worth asserting.
        TAP_CHECK(low + CHURN_MIN_FRAMES <= frames0);
        TAP_CHECK(frames1 == frames0);
        TAP_CHECK(roots1 == roots0);
        // The refusal counter, folded in: no path above handed the pool a frame twice.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- A THREAD'S STACK IS FRAMES, WITH A GUARD PAGE, AND THEY COME BACK -----------
    // The whole of what an arena block cannot be (docs/design-m6-mmu.md section 3.4). Two
    // things no other arm can say. A LIVE worker holds frames: an arena stack costs the pool
    // nothing, so the drop is what separates the two allocators. And a DEAD one has given
    // every one of them back, guard page included, which is the release path no balance arm
    // above reaches because none of them spawns a thread.
    //
    // A WARM-UP CYCLE FIRST, and it is not padding: the intermediate tables a stack's
    // virtual range needs are allocated on the first map into that range and are
    // deliberately never pruned, so a first cycle spends frames a second one does not. Only
    // the second can balance, and a warm-up-free version of this arm reads as a leak.
    kos_cap_t g_sfgate = KOS_CAP_NONE;
    void sframe_worker(void*) // caps: done@1, gate@2
    {
        kos_sem_post(CH_DONE);
        kos_sem_wait(2); // held alive while root reads the pool
    }
    bool sframe_cycle(uintptr_t* live)
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_sfgate, CH_FULL}};
        auto w = kos::thread::create_caps(sframe_worker, nullptr, "sfram", 10, caps, 2);
        if (not w.valid())
        {
            return false;
        }
        wait_n(1);
        if (live != nullptr)
        {
            *live = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        }
        kos_sem_post(g_sfgate);
        return w.join() == 0;
    }
    void t_stack_is_frames()
    {
        TAP_CHECK(kos_sem_create(0, &g_sfgate) == 0);
        TAP_CHECK(sframe_cycle(nullptr));
        uintptr_t const before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        uintptr_t live = 0;
        TAP_CHECK(sframe_cycle(&live));
        // At least the stack and the unmapped page below it.
        TAP_CHECK(before >= live + 2);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) == before);
        tap::diag("stack frames: %u held by one live thread",
                  static_cast<unsigned>(before - live));
        // T4's rule, read AFTER the release path above has run: a space frees what it maps,
        // the borrower unmaps first, and the pool has refused no free. The balance op answers
        // with the refusal COUNTER folded in, and that counter is cumulative, so this is the
        // one place it covers a stack that was unmapped and handed back.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
        TAP_CHECK(kos_handle_close(g_sfgate) == 0);
        g_sfgate = KOS_CAP_NONE;
    }

    // --- Two tasks, two address spaces (docs/design-m6-mmu.md F2) -----------------
    // BOTH workers are spawned before either runs, so the two domains coexist and a slot
    // freed by an early exit cannot be handed to the second and answer with the first's
    // name.
    // EACH WORKER IS ITS OWN PROCESS, so it cannot answer through an app global: its static
    // data is a copy of root's (section 3.4). `arg` points at the slot it writes, inside
    // memory root and the worker both map, and root reads it back at the address root named.
    void space_id_worker(void* arg)
    {
        uint32_t const id = static_cast<uint32_t>(kos_aspace_probe(KOS_ASPACE_OP_SPACE_ID, 0));
        *static_cast<volatile uint32_t*>(arg) = id;
        kos_sem_post(CH_DONE);
    }

    // Spawns the pair and answers false when the pool could not seat both. Whichever worker
    // DID spawn is still waited for: g_done is shared with every later arm, so an unclaimed
    // post would arrive inside one of theirs.
    bool two_space_ids(void* mem_base, uint32_t mem_size, volatile uint32_t* slot)
    {
        slot[0] = 0;
        slot[1] = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        int seated = 0;
        if (kos::thread::create_caps(space_id_worker, const_cast<uint32_t*>(&slot[0]), "spidA",
                                     10, caps, 1, KOS_POLICY_FIFO, 0, false, mem_base,
                                     mem_size).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(space_id_worker, const_cast<uint32_t*>(&slot[1]), "spidB",
                                     10, caps, 1, KOS_POLICY_FIFO, 0, false, mem_base,
                                     mem_size).valid())
        {
            seated++;
        }
        wait_n(seated);
        return seated == 2;
    }

    void t_aspace_two_spaces_same_grant()
    {
        void* const shared = kos_ram_alloc(256);
        if (shared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // Root's own view of the block the pair is handed, which is where they answer.
        TAP_CHECK(kos_mem_self_grant(shared, 256, 0) == 0);
        volatile uint32_t* const slot = static_cast<volatile uint32_t*>(shared);
        if (not two_space_ids(shared, 256, slot))
        {
            tap::skip("thread pool too small for 2 concurrent");
            return;
        }
        uint32_t const ida = slot[0];
        uint32_t const idb = slot[1];
        TAP_CHECK(ida != 0 and idb != 0);
        TAP_CHECK(ida != idb);
    }

    // NOTHING GRANTED MEANS NO SHARED BYTE, so this pair answers down an ENDPOINT: neither
    // member touches root's static data, of which each holds a copy of its own.
    void space_id_ep_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        uint32_t const id = static_cast<uint32_t>(kos_aspace_probe(KOS_ASPACE_OP_SPACE_ID, 0));
        (void)kos_send(2, &id, sizeof(id));
        kos_sem_post(CH_DONE);
    }

    // The no-grant pair, one member each in a task of its OWN. Two EXPLICIT creates and not
    // two plain spawns: a plain spawn is a thread of the CALLER's task, so two of them are
    // siblings in root's space and would answer one id however F2 resolves the domain. Both
    // groups exist before either member runs, so the two domains coexist.
    bool two_space_ids_own_tasks(uint32_t* ida, uint32_t* idb)
    {
        *ida = 0;
        *idb = 0;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            return false;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &ta) != 0)
        {
            (void)kos_handle_close(ep);
            return false;
        }
        if (kos_task_create(nullptr, 0, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(ep);
            return false;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        int seated = 0;
        if (kos::thread::create_caps(space_id_ep_worker, nullptr, "spidA", 10, caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     ta).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(space_id_ep_worker, nullptr, "spidB", 10, caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                     tb).valid())
        {
            seated++;
        }
        // One receive per SEATED member: a member parks in its send until root arrives, and
        // only then posts, so draining first would wait on a post that cannot happen.
        for (int i = 0; i < seated; i++)
        {
            uint32_t id = 0;
            if (kos_recv(ep, &id, sizeof(id), nullptr) != static_cast<int32_t>(sizeof(id)))
            {
                break;
            }
            if (i == 0)
            {
                *ida = id;
            }
            else
            {
                *idb = id;
            }
        }
        wait_n(seated);
        // After the members are gone, so each kill only drops root's hold on an empty group.
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(ep);
        return seated == 2;
    }

    void t_aspace_two_spaces_no_grant()
    {
        // The case the arm above misses: two tasks with nothing granted, which must still be
        // two address spaces of their own (F2).
        uint32_t ida = 0;
        uint32_t idb = 0;
        if (not two_space_ids_own_tasks(&ida, &idb))
        {
            tap::skip("thread, task or endpoint pool too small for 2 concurrent");
            return;
        }
        TAP_CHECK(ida != 0 and idb != 0);
        TAP_CHECK(ida != idb);
    }

    // --- THE PROCESS WITNESS (docs/design-m6-mmu.md F2, section 3.4) -----------------
    // Two tasks whose TEXT and DATA sit at the same virtual addresses, backed by DIFFERENT
    // frames for the data and by ONE frame for the text, each reading and writing its own.
    //
    // THE FRAMES ARE COMPARED AND NOT JUST THE BEHAVIOUR. Two members that merely run prove
    // nothing an identity map would not; two members answering different frame numbers for
    // one address are two copies of that page, and answering the same one for a text address
    // is the sharing that makes the copy the deliberate half.
    //
    // Each member is handed a block of its own, which is how it answers at all: its static
    // data IS the thing under test, so a report through an app global would be measuring the
    // instrument.
    enum
    {
        PW_ADDR = 0,     // the member's own &g_pw_word
        PW_DATA_FRAME = 1,
        PW_TEXT_FRAME = 2,
        PW_READBACK = 3,
        PW_SEED = 4, // root's, read by the member: the value only that member writes
        PW_WORDS = 5
    };
    volatile uint32_t g_pw_word = 0u;
    void pw_member(void* arg) // caps: done@1, gate@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        uint32_t const mine = static_cast<uint32_t>(out[PW_SEED]);
        out[PW_ADDR] = reinterpret_cast<uintptr_t>(&g_pw_word);
        out[PW_DATA_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&g_pw_word));
        out[PW_TEXT_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&pw_member));
        g_pw_word = mine;
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_LOCK); // the gate, delegated second; both members have written by now
        out[PW_READBACK] = g_pw_word;
        kos_sem_post(CH_DONE);
    }
    void t_process_private_data()
    {
        constexpr uint32_t PW_BLK = 256;
        constexpr uint32_t PW_A = 0xA5A50F0Fu;
        constexpr uint32_t PW_B = 0x5A5AF0F0u;
        void* const ba = kos_ram_alloc(PW_BLK);
        void* const bb = kos_ram_alloc(PW_BLK);
        if (ba == nullptr or bb == nullptr)
        {
            tap::skip("arena cannot spare two report blocks");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(ba, PW_BLK, 0) == 0);
        TAP_CHECK(kos_mem_self_grant(bb, PW_BLK, 0) == 0);
        volatile uint64_t* const oa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const ob = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PW_WORDS; i++)
        {
            oa[i] = 0;
            ob[i] = 0;
        }
        oa[PW_SEED] = PW_A;
        ob[PW_SEED] = PW_B;
        g_pw_word = 0x600Du;
        kos_cap_t gate = KOS_CAP_NONE;
        if (kos_sem_create(0, &gate) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(ba, PW_BLK, 0, &ta) != 0 or kos_task_create(bb, PW_BLK, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(gate);
            tap::skip("task pool too small for 2 groups");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {gate, CH_FULL}};
        int seated = 0;
        if (kos::thread::create_caps(pw_member, ba, "pwA", 10, caps, 2, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, 0, nullptr, ta).valid())
        {
            seated++;
        }
        if (kos::thread::create_caps(pw_member, bb, "pwB", 10, caps, 2, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, 0, nullptr, tb).valid())
        {
            seated++;
        }
        if (seated != 2)
        {
            // Whichever member did start is parked on the gate; release it so the group can
            // be reaped and its post cannot land inside a later arm.
            wait_n(seated);
            kos_sem_post(gate);
            wait_n(seated);
            (void)kos_task_kill(ta);
            (void)kos_task_kill(tb);
            (void)kos_handle_close(gate);
            tap::skip("thread pool too small for 2 concurrent");
            return;
        }
        wait_n(2); // both have written their own copy
        kos_sem_post(gate);
        kos_sem_post(gate);
        wait_n(2); // both have read it back
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(gate);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pw_word);
        uint64_t const rootdf =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, own);
        uint64_t const roottf =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&pw_member));
        tap::diag("process: data frames %u/%u/%u, text frame %u",
                  static_cast<unsigned>(rootdf), static_cast<unsigned>(oa[PW_DATA_FRAME]),
                  static_cast<unsigned>(ob[PW_DATA_FRAME]), static_cast<unsigned>(roottf));
        // ONE address in all three spaces.
        TAP_CHECK(oa[PW_ADDR] == own and ob[PW_ADDR] == own);
        // THREE frames under it, which is what a per-process copy means.
        TAP_CHECK(rootdf != 0 and oa[PW_DATA_FRAME] != 0 and ob[PW_DATA_FRAME] != 0);
        TAP_CHECK(oa[PW_DATA_FRAME] != ob[PW_DATA_FRAME]
                  and oa[PW_DATA_FRAME] != rootdf and ob[PW_DATA_FRAME] != rootdf);
        // ONE frame under the text, which is the sharing the copy is measured against.
        TAP_CHECK(roottf != 0 and oa[PW_TEXT_FRAME] == roottf and ob[PW_TEXT_FRAME] == roottf);
        // Each read back its OWN write, after the other had written the same address.
        TAP_CHECK(oa[PW_READBACK] == PW_A and ob[PW_READBACK] == PW_B);
        TAP_CHECK(g_pw_word == 0x600Du); // and root's copy was untouched by either
    }

    // --- Two members of ONE group share their image (F2's sibling witness) -----------
    // The arm domain_share was mistaken for, and it is observable only now: before the
    // per-process data copy every task read the one set of app-data frames, so two members
    // agreeing said nothing about their group.
    volatile uint32_t g_sib_word = 0u;
    void sib_writer(void* arg) // caps: done@1
    {
        g_sib_word = 0xBEEFu;
        static_cast<volatile uint64_t*>(arg)[0] = 1u;
        kos_sem_post(CH_DONE);
    }
    void sib_reader(void* arg) // caps: done@1
    {
        static_cast<volatile uint64_t*>(arg)[1] = g_sib_word;
        kos_sem_post(CH_DONE);
    }
    void t_task_siblings_share()
    {
        constexpr uint32_t SIB_BLK = 256;
        void* const blk = kos_ram_alloc(SIB_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, SIB_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[0] = 0;
        out[1] = 0;
        g_sib_word = 0u;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(blk, SIB_BLK, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // STRICTLY ORDERED, one member at a time: the reader must run after the writer, and a
        // spawn does not preempt, so the writer is drained before the reader is seated.
        if (not kos::thread::create_caps(sib_writer, blk, "sibW", 10, caps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        if (not kos::thread::create_caps(sib_reader, blk, "sibR", 10, caps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t);
        TAP_CHECK(out[0] == 1u);          // the writer ran
        TAP_CHECK(out[1] == 0xBEEFu);     // and its sibling saw the store: one image, one group
        TAP_CHECK(g_sib_word == 0u);      // root did not: a different group is a different copy
    }

    // --- The handoff readback, both consumers (F10) ---------------------------------
    // Root reserves, self-grants, WRITES, and hands the block over; the receiver reads back
    // what root wrote AT THE SAME ADDRESS. It is what the two-handoff witness F2 keeps
    // became: the pair below shares no app global, one receiver arriving through a task
    // create and the other through a grant-carrying spawn.
    //
    // NOT A STAND-IN FOR THE DRIVER BRING-UP FLOW, which F10 makes the gate for this ABI and
    // says in terms no selftest arm substitutes for. That flow runs the same four steps, but
    // it does not run on a board that declares no service list, and this arm does not make it
    // run.
    enum
    {
        HO_SEEN = 0,
        HO_ADDR = 1,
        HO_FRAME = 2,
        HO_TYPE = 3, // 1 + the memory type this space's mapping of the block carries
        HO_WORDS = 4
    };
    constexpr uint64_t HO_SENTINEL = 0x0D15EA5Eu;
    void ho_reader(void* arg) // caps: done@1
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        uint64_t const seen = blk[HO_SEEN];
        blk[HO_ADDR] = reinterpret_cast<uintptr_t>(arg);
        // One frame under two spaces, which a target given frames of its own would answer
        // differently.
        blk[HO_FRAME] = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(arg));
        blk[HO_TYPE] =
            kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, reinterpret_cast<uintptr_t>(arg));
        blk[HO_SEEN] = seen + 1u; // the readback, echoed back through the same bytes
        kos_sem_post(CH_DONE);
    }
    void t_task_handoff_readback()
    {
        constexpr uint32_t HO_BLK = 256;
        void* const blk = kos_ram_alloc(HO_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare the shared block");
            return;
        }
        // Reach it before writing it: allocation grants nothing (F10).
        TAP_CHECK(kos_mem_self_grant(blk, HO_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        out[HO_FRAME] = 0;
        uint64_t const mine =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(blk));
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(blk, HO_BLK, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // CONSUMER ONE: an explicit create, which is the driver framework's own path.
        if (not kos::thread::create_caps(ho_reader, blk, "hoT", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t);
        uint64_t const first_seen = out[HO_SEEN];
        uint64_t const first_addr = out[HO_ADDR];
        uint64_t const first_frame = out[HO_FRAME];
        out[HO_SEEN] = HO_SENTINEL;
        out[HO_ADDR] = 0;
        out[HO_FRAME] = 0;
        // CONSUMER TWO: a spawn carrying the same range as its own grant, which takes a task
        // of its own and reaches the block through the same handoff by another syscall.
        if (not kos::thread::create_caps(ho_reader, blk, "hoS", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, blk, HO_BLK).valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        TAP_CHECK(mine != 0 and first_frame == mine and out[HO_FRAME] == mine);
        TAP_CHECK(first_seen == HO_SENTINEL + 1u);   // the child read what root wrote
        TAP_CHECK(first_addr == reinterpret_cast<uintptr_t>(blk)); // at the address root named
        TAP_CHECK(out[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(out[HO_ADDR] == reinterpret_cast<uintptr_t>(blk));
        // --- THE FLAGS-MATCH RULE, on a block of its own --------------------------------
        // The memory type belongs to the BLOCK (kos_mem_flags): every call that maps it must
        // be passed the same one, or its two live mappings disagree about the type. A block
        // handed over with flags 0 everywhere never asks that question, so this leg carries a
        // non-zero type end to end and both sides report what their mapping recorded.
        //
        // A BLOCK OF ITS OWN, and not the pair above, because a mismatched alias is exactly
        // what the rule forbids: the spawn consumer below cannot be given a type at all, so
        // reusing one block would have built the disagreement rather than the agreement.
        //
        // THE CREATE CONSUMER ONLY, and the reason is the ABI. A grant-carrying spawn has no
        // memory-type field, so task_for passes 0 and that consumer maps Normal whatever the
        // donor holds; no caller can obey the rule through it. It is recorded at F10 rather
        // than worked around here.
        void* const typed = kos_ram_alloc(HO_BLK);
        if (typed == nullptr)
        {
            tap::skip("no reservation left for the typed handoff");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(typed, HO_BLK, KOS_MEM_NOCACHE) == 0);
        volatile uint64_t* const tout = static_cast<volatile uint64_t*>(typed);
        tout[HO_SEEN] = HO_SENTINEL;
        tout[HO_ADDR] = 0;
        tout[HO_TYPE] = 0;
        // 1 + ARCH_MAP_NOCACHE, the non-cacheable type KOS_ASPACE_OP_MEMTYPE numbers 1.
        constexpr uint64_t HO_NOCACHE_AT = 2u;
        uint64_t const donor_type =
            kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, reinterpret_cast<uintptr_t>(typed));
        kos_task_t tt = KOS_TASK_NONE;
        if (kos_task_create(typed, HO_BLK, KOS_MEM_NOCACHE, &tt) != 0)
        {
            tap::skip("task pool too small for the typed handoff");
            return;
        }
        if (not kos::thread::create_caps(ho_reader, typed, "hoN", 10, caps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, tt).valid())
        {
            (void)kos_task_kill(tt);
            tap::skip("thread pool too small for the typed handoff");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(tt);
        TAP_CHECK(donor_type == HO_NOCACHE_AT);
        TAP_CHECK(tout[HO_TYPE] == donor_type); // the two live mappings agree
        TAP_CHECK(tout[HO_SEEN] == HO_SENTINEL + 1u);
        TAP_CHECK(tout[HO_ADDR] == reinterpret_cast<uintptr_t>(typed));
        // Both borrowers are gone and root still maps the block: a space frees what it MAPS
        // and the borrower unmaps first, so nothing here may have handed the pool a frame it
        // does not own (T4, F10). All ones is what a refused free reads as.
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- T4's ownership rule the other way round: THE DONOR DIES FIRST ---------------
    // T4 says the borrower unmaps before it dies, and every arm above stages exactly that:
    // task_handoff_readback kills both borrowers while root, the donor, runs on. The reverse
    // order is what frees frames under a live mapping, and NO FRAME-POOL COUNTER CAN SEE IT:
    // a donor that dies first frees each of its frames exactly once, so frame_pool_refused
    // stays zero while the borrower reads memory the pool has since handed to somebody else.
    // The lifetime edge domain_for takes on the donor (kernel/domain/domain.cc) is what this
    // arm holds to account.
    //
    // THE DONOR IS A TASK OF ITS OWN, because root cannot exit. It reserves a block of its
    // OWN and hands that one over: a block ROOT reserved is BORROWED in the donor's space
    // too, so the donor's teardown would free none of it and there would be nothing to catch.
    // Root joins the donor's thread, so the donor is gone before anything below runs.
    //
    // A CHURN TASK THEN TAKES WHAT THE POOL WILL HAND OUT and stamps every frame of it, so a
    // block released under the borrower's feet reads back as somebody else's bytes rather
    // than as the donor's.
    enum
    {
        DX_STATUS = 0, // 1 once the donor seated a borrower into its own block
        DX_ADDR = 1,   // the donor's address for that block
        DX_TOKEN = 2,  // the frame under it, named in the donor's own space
        DX_HELD = 3,   // spaces held while the donor was still alive
        DX_WORDS = 4
    };
    enum
    {
        CX_TAKEN = 0,  // blocks the churn task got
        CX_TOKEN0 = 1, // one word per block: the frame it landed on
        CX_MAX = 24
    };
    constexpr uint32_t DX_BLK = 64;
    constexpr uint32_t DX_CBLK = 256;
    constexpr uint64_t DX_DONOR_WORD = 0xD0D0D0D0D0D0D0D0ull;
    constexpr uint64_t DX_BORROW_WORD = 0xB0B0B0B0B0B0B0B0ull;
    constexpr uint64_t DX_CHURN_WORD = 0xC0C0C0C0C0C0C0C0ull;
    constexpr uint32_t DX_CALL_US = 250000;
    constexpr int DX_EP = 2; // the donor's and the borrower's endpoint index
    struct DxReport
    {
        uint64_t seen;     // what the borrower found in the block
        uint64_t readback; // what it read after writing its own word over it
        uint64_t token;    // the frame under the block, named in the BORROWER's space
        uint64_t addr;
    };

    // Parks on the endpoint until root releases it, which is after the donor is gone AND
    // after the churn task has taken everything the pool would hand out.
    void dx_borrower(void* arg) // caps: done@1, ep(WAIT)@2
    {
        volatile uint64_t* const blk = static_cast<volatile uint64_t*>(arg);
        char msg[sizeof(DxReport)] = {};
        struct kos_recv_info info = {0u, KOS_CAP_NONE};
        int32_t const got = kos_recv(DX_EP, msg, sizeof(msg), &info);
        DxReport r = {};
        r.seen = blk[0];
        blk[0] = DX_BORROW_WORD; // and WRITES it: a stale mapping is writable, not only readable
        r.readback = blk[0];
        r.token = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(arg));
        r.addr = reinterpret_cast<uintptr_t>(arg);
        if (got >= 0)
        {
            memcpy(msg, &r, sizeof(r));
            (void)kos_reply(info.reply_cap, msg, sizeof(msg));
        }
        kos_sem_post(CH_DONE);
    }

    // Reserves a block of its own, hands it to a borrower it creates, and returns. Its own
    // exit is the whole point, so it posts no completion: root joins it.
    void dx_donor(void* arg) // caps: done@1, ep(WAIT|TRANSFER)@2; arg is ROOT's report block
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        void* const blk = kos_ram_alloc(DX_BLK);
        if (blk == nullptr or kos_mem_self_grant(blk, DX_BLK, 0) != 0)
        {
            return;
        }
        volatile uint64_t* const mine = static_cast<volatile uint64_t*>(blk);
        mine[0] = DX_DONOR_WORD;
        out[DX_ADDR] = reinterpret_cast<uintptr_t>(blk);
        out[DX_TOKEN] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(blk));
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(blk, DX_BLK, 0, &tb) != 0)
        {
            return;
        }
        kos_cap_grant caps[] = {{CH_DONE, CH_FULL}, {DX_EP, KOS_CAP_WAIT}};
        if (not kos::thread::create_caps(dx_borrower, blk, "dxB", 10, caps, 2, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, 0, nullptr, tb)
                    .valid())
        {
            return;
        }
        // Read LAST, and inside the donor, so the number counts the donor's own space. Root
        // compares it after the join: nothing else claims or frees a space in between, so an
        // equal number means the donor's space outlived its last task and a smaller one means
        // it did not.
        out[DX_HELD] = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);
        out[DX_STATUS] = 1;
    }

    // Takes one-granule blocks until the pool or its own range table says no, stamping each,
    // and records the frame every one of them landed on.
    void dx_churn(void* arg) // caps: done@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        size_t const g = discover_granule();
        uint64_t taken = 0;
        while (g != 0 and taken < static_cast<uint64_t>(CX_MAX))
        {
            void* const p = kos_ram_alloc(static_cast<uint32_t>(g));
            if (p == nullptr or kos_mem_self_grant(p, static_cast<uint32_t>(g), 0) != 0)
            {
                break;
            }
            *static_cast<volatile uint64_t*>(p) = DX_CHURN_WORD;
            out[CX_TOKEN0 + taken] =
                kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(p));
            taken++;
        }
        out[CX_TAKEN] = taken;
        kos_sem_post(CH_DONE);
    }

    void t_task_handoff_donor_exits()
    {
        void* const dblk = kos_ram_alloc(DX_BLK);
        void* const cblk = kos_ram_alloc(DX_CBLK);
        if (dblk == nullptr or cblk == nullptr)
        {
            tap::skip("arena cannot spare the two report blocks");
            return;
        }
        if (kos_mem_self_grant(dblk, DX_BLK, 0) != 0
            or kos_mem_self_grant(cblk, DX_CBLK, 0) != 0)
        {
            tap::skip("the report blocks are not reachable");
            return;
        }
        volatile uint64_t* const dout = static_cast<volatile uint64_t*>(dblk);
        volatile uint64_t* const cout = static_cast<volatile uint64_t*>(cblk);
        for (int i = 0; i < DX_WORDS; i++)
        {
            dout[i] = 0;
        }
        for (int i = 0; i < CX_TOKEN0 + CX_MAX; i++)
        {
            cout[i] = 0;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no endpoint slot for the borrower's report");
            return;
        }
        kos_task_t td = KOS_TASK_NONE;
        if (kos_task_create(dblk, DX_BLK, 0, &td) != 0)
        {
            (void)kos_handle_close(ep);
            tap::skip("task pool too small for the donor");
            return;
        }
        // TRANSFER on the endpoint, because the donor does not use it: it re-delegates it to
        // the borrower, which is the only thread that may answer root.
        kos_cap_grant dcaps[] = {{g_done, CH_FULL},
                                 {ep, static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_TRANSFER)}};
        auto donor = kos::thread::create_caps(dx_donor, dblk, "dxD", 10, dcaps, 2,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                              KOS_AUTH_MEMORY, nullptr, td);
        if (not donor.valid())
        {
            (void)kos_task_kill(td);
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small for the donor");
            return;
        }
        // THE ORDERING THE WHOLE ARM IS ABOUT: the donor is GONE from here down.
        int const jrc = donor.join(DX_CALL_US);
        bool const seated = jrc == 0 and dout[DX_STATUS] == 1u;
        // AND ROOT'S OWN HOLD GOES WITH IT, here and not in the cleanup below. kos_task_create
        // takes a reference on the new task's domain for its creator, so root joining the
        // donor's thread is NOT the donor's last reference: without this the donor's domain
        // never reaches zero and the whole ordering this arm stages is unreachable.
        (void)kos_task_kill(td);
        if (not seated)
        {
            (void)kos_handle_close(ep);
            tap::skip("the donor could not seat a borrower");
            return;
        }
        uint64_t const held_after = kos_aspace_probe(KOS_ASPACE_OP_SPACES_HELD, 0);

        kos_task_t tc = KOS_TASK_NONE;
        uint64_t churned = 0;
        if (kos_task_create(cblk, DX_CBLK, 0, &tc) == 0)
        {
            kos_cap_grant ccaps[] = {{g_done, CH_FULL}};
            if (kos::thread::create_caps(dx_churn, cblk, "dxC", 10, ccaps, 1, KOS_POLICY_FIFO,
                                         0, false, nullptr, 0, KOS_AUTH_MEMORY, nullptr, tc)
                    .valid())
            {
                wait_n(1);
                churned = cout[CX_TAKEN];
            }
            (void)kos_task_kill(tc);
        }

        // Releases the borrower and takes its answer in one round trip, so a borrower that
        // never parked fails this arm instead of hanging the suite.
        char msg[sizeof(DxReport)] = {};
        int32_t const n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), DX_CALL_US);
        DxReport r = {};
        if (n == static_cast<int32_t>(sizeof(r)))
        {
            memcpy(&r, msg, sizeof(r));
        }
        wait_n(1);
        (void)kos_handle_close(ep);

        // No frame the pool handed the churn task may be the one the borrower still maps.
        bool handed_out = false;
        uint64_t clo = 0;
        uint64_t chi = 0;
        for (uint64_t i = 0; i < churned and i < static_cast<uint64_t>(CX_MAX); i++)
        {
            uint64_t const tok = cout[CX_TOKEN0 + i];
            if (tok == dout[DX_TOKEN])
            {
                handed_out = true;
            }
            if (clo == 0 or tok < clo)
            {
                clo = tok;
            }
            if (tok > chi)
            {
                chi = tok;
            }
        }
        // The churn window is reported because the token check is only as strong as its
        // coverage: a donor frame outside [clo, chi] was never offered to anybody, and then
        // this arm rests on the two checks that do not depend on reuse.
        tap::diag("donor-exits: donor frame %u, borrower frame %u, spaces held %u -> %u",
                  static_cast<unsigned>(dout[DX_TOKEN]), static_cast<unsigned>(r.token),
                  static_cast<unsigned>(dout[DX_HELD]), static_cast<unsigned>(held_after));
        tap::diag("donor-exits: borrower read %u, churn took %u blocks over frames %u..%u",
                  static_cast<unsigned>(r.seen), static_cast<unsigned>(churned),
                  static_cast<unsigned>(clo), static_cast<unsigned>(chi));
        // THE LIFETIME EDGE, and the one check that does not depend on which frame the pool
        // reused: the donor's last task is gone and its space is still held, by the borrower's
        // reference on its domain.
        TAP_CHECK(held_after == dout[DX_HELD]);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the borrower answered at all
        TAP_CHECK(r.addr == dout[DX_ADDR]);              // at the donor's address
        TAP_CHECK(r.token != 0 and r.token == dout[DX_TOKEN]); // on the donor's frame
        // THE BYTES. A donor whose frames went back to the pool leaves the churn task's word
        // here, or its copied globals, and never the donor's own.
        TAP_CHECK(r.seen == DX_DONOR_WORD);
        TAP_CHECK(r.readback == DX_BORROW_WORD);
        TAP_CHECK(not handed_out);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- F10's CROSS-TASK self-grant refusal ------------------------------------------
    // A reservation names frames of the RESERVING task's own space, so an address another
    // task reserved is meaningless here rather than merely unreachable, and the self-grant
    // must refuse it BY NAME. The worker is a task of its own and answers down an ENDPOINT:
    // it shares no byte with root, which is the situation under test.
    //
    // TWO CODES, and the second is what makes the first about the ADDRESS. The same worker,
    // through the same call, self-granting a range IT reserved must succeed; without that
    // control, a -KOS_EPERM from a missing authority or an unbuilt path reads as the refusal.
    enum
    {
        XG_OWN = 0,     // the worker's own reservation: granted
        XG_FOREIGN = 1, // root's reservation, named from another space: refused
        XG_WORDS = 2
    };
    constexpr uint32_t XG_BLK = 64;
    void xg_worker(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        int32_t rep[XG_WORDS] = {1, 1};
        void* const mine = kos_ram_alloc(XG_BLK);
        if (mine != nullptr)
        {
            rep[XG_OWN] = kos_mem_self_grant(mine, XG_BLK, 0);
        }
        // Root's address, carried as a NUMBER and never dereferenced: this space does not
        // map it, and the point is that it cannot make it map.
        rep[XG_FOREIGN] = kos_mem_self_grant(arg, XG_BLK, 0);
        (void)kos_send(2, rep, sizeof(rep));
        kos_sem_post(CH_DONE);
    }
    void t_self_grant_cross_task()
    {
        void* const theirs = kos_ram_alloc(XG_BLK);
        if (theirs == nullptr)
        {
            tap::skip("arena cannot spare the donor block");
            return;
        }
        // Root maps it, so the worker names a range that really is live somewhere.
        TAP_CHECK(kos_mem_self_grant(theirs, XG_BLK, 0) == 0);
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(ep);
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        if (not kos::thread::create_caps(xg_worker, theirs, "xgrnt", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                         KOS_AUTH_MEMORY, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small");
            return;
        }
        int32_t rep[XG_WORDS] = {1, 1};
        bool const heard =
            kos_recv(ep, rep, sizeof(rep), nullptr) == static_cast<int32_t>(sizeof(rep));
        wait_n(1);
        (void)kos_task_kill(t);
        (void)kos_handle_close(ep);
        TAP_CHECK(heard);
        TAP_CHECK(rep[XG_OWN] == 0);
        TAP_CHECK(rep[XG_FOREIGN] == -KOS_EPERM);
    }

    // --- F10's teardown release --------------------------------------------------------
    // A reservation that was never self-granted has NO LEAF pointing at its frames, so the
    // destroy walk cannot see them and aspace_release is the only path that hands them back.
    // Only a process that DIES holding one can show it: every other reservation in this suite
    // is root's, and root outlives the run.
    //
    // A WARM-UP CYCLE FIRST, for the reason t_stack_is_frames states.
    enum
    {
        RT_ADDR = 0, // the reservation the member took and never mapped
        RT_FREE = 1, // frames free with it out, read INSIDE the live process
        RT_WORDS = 2
    };
    constexpr uintptr_t RT_PAGES = 3;
    void rt_member(void*) // caps: E(SIGNAL)@1
    {
        uint64_t rep[RT_WORDS] = {0, 0};
        uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
        // RESERVED AND NEVER GRANTED, which is the state teardown has to answer for.
        void* const blk = kos_ram_alloc(static_cast<size_t>(RT_PAGES * g));
        rep[RT_ADDR] = reinterpret_cast<uintptr_t>(blk);
        rep[RT_FREE] = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_send(1, rep, sizeof(rep));
    }
    bool rt_cycle(kos_cap_t ep, uint64_t* rep)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            return false;
        }
        kos_cap_grant caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto m = kos::thread::create_caps(rt_member, nullptr, "rtres", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY, nullptr, t);
        if (not m.valid())
        {
            (void)kos_task_kill(t);
            return false;
        }
        // The member parks in its send until this receive arrives, so the report is read
        // while the reservation is still out.
        bool const heard = kos_recv(ep, rep, sizeof(uint64_t) * RT_WORDS, nullptr)
                           == static_cast<int32_t>(sizeof(uint64_t) * RT_WORDS);
        bool const joined = m.join(CHURN_JOIN_US) == 0;
        // EMPTIED IS NOT DEAD: root's creator hold is what still holds the space open.
        bool const reaped = kos_task_kill(t) == 0;
        return heard and joined and reaped;
    }
    void t_reservation_teardown()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        uint64_t rep[RT_WORDS] = {0, 0};
        if (not rt_cycle(ep, rep))
        {
            (void)kos_handle_close(ep);
            tap::skip("task or thread pool too small to churn a reservation");
            return;
        }
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        bool const ran = rt_cycle(ep, rep);
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        (void)kos_handle_close(ep);
        TAP_CHECK(ran);
        TAP_CHECK(rep[RT_ADDR] != 0);
        tap::diag("reservation teardown: %u frames free, %u inside the live process, %u after",
                  static_cast<unsigned>(before), static_cast<unsigned>(rep[RT_FREE]),
                  static_cast<unsigned>(after));
        // THE COUNT MOVED, so the return is not vacuous: a reservation is frames the pool
        // handed out, and a process that reserved nothing balances trivially.
        TAP_CHECK(rep[RT_FREE] + RT_PAGES <= before);
        TAP_CHECK(after == before);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- A sibling scribbling a PARKED member's frame (docs/design-m6-mmu.md F9, T6) ----
    // Thread stacks are TASK-WIDE mappings here, so a member can write a not-yet-run
    // member's stack. What must not be reachable there is the privileged return state: the
    // saved processor state and the return address that the one exception return starting
    // the victim consumes. The sibling fills the whole top-of-stack frame window with the
    // AArch64 EL1h state word, so whichever slot holds the saved state reads "resume
    // privileged" and whichever holds the return address reads a value the sibling picked.
    // FILLING THE WINDOW rather than two known offsets is what keeps the arm from going
    // vacuous when the frame layout moves.
    //
    // The victim proves both halves. Reaching its own body says the return address was its
    // entry; kos_shutdown answering -KOS_EPERM says it runs unprivileged. Were it privileged
    // that call would SUCCEED and end the run, which the TAP gate reads as a truncated
    // stream, so the arm cannot pass by escalating quietly.
    //
    // THE SIBLING MUST NOT EXIT WHILE THE VICTIM IS PARKED: it writes the victim's saved
    // frame, so both have to be live at once. It parks on a semaphore nothing posts, and the
    // victim's own exit collects it.
    constexpr uint64_t HOSTILE_EL1H = 0x205u; // M[3:0] = EL1h, plus the debug mask
    constexpr uint32_t HOSTILE_WINDOW = 1024; // spans the whole armv8a exception frame
    constexpr uint32_t HOSTILE_JOIN_US = 60000;
    constexpr int CH_HPARK = 2; // delegated SECOND to the sibling
    // The victim is a member of a group of its own, so it is a process and its app globals
    // are a copy of root's: its verdict goes into the block root handed the group, which root
    // reads back at the address root named (docs/design-m6-mmu.md F10).
    void hostile_victim(void* arg)
    {
        *static_cast<volatile int32_t*>(arg) = kos_shutdown(0);
        kos_exit(0);
    }
    void hostile_sibling(void* arg) // caps: done@1, park@2
    {
        uintptr_t const top = reinterpret_cast<uintptr_t>(arg);
        volatile uint64_t* const w = reinterpret_cast<volatile uint64_t*>(top - HOSTILE_WINDOW);
        for (uint32_t i = 0; i < HOSTILE_WINDOW / sizeof(uint64_t); i++)
        {
            w[i] = HOSTILE_EL1H;
        }
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_HPARK);
        kos_exit(1); // unreachable: nothing posts that semaphore
    }
    void t_parked_frame_hostile()
    {
#if defined(KICKOS_TLS) && KICKOS_TLS
        // The TLS seat admits a caller stack of exactly one stride, stride-aligned.
        constexpr uint32_t VSTK = KICKOS_TLS_STRIDE;
#else
        constexpr uint32_t VSTK = 8192;
#endif
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        // Twice the stack, so the aligned block fits wherever the allocator lands, plus one
        // stride above it for the verdict slot, which is therefore never inside the stack.
        void* const raw = kos_ram_alloc(3u * VSTK);
        if (raw == nullptr)
        {
            (void)kos_handle_close(park);
            tap::skip("arena cannot spare a strided victim stack");
            return;
        }
        // Root's own view of the block it is about to hand over: without it root reserved
        // the range and mapped nothing, and the verdict slot below is unreachable here.
        TAP_CHECK(kos_mem_self_grant(raw, 3u * VSTK, 0) == 0);
        uintptr_t const vbase = (reinterpret_cast<uintptr_t>(raw) + (VSTK - 1u))
            & ~static_cast<uintptr_t>(VSTK - 1u);
        volatile int32_t* const verdict =
            reinterpret_cast<volatile int32_t*>(reinterpret_cast<uintptr_t>(raw) + 2u * VSTK);
        *verdict = -99;
        kos_task_t task = KOS_TASK_NONE;
        // THE GROUP IS HANDED THE BLOCK, which is what maps the victim's stack and the
        // verdict slot in the space the group runs in, at the addresses root named.
        TAP_CHECK(kos_task_create(raw, 3u * VSTK, 0, &task) == 0);
        // PRIORITY 1 IS BELOW ROOT'S OWN, so the victim cannot be scheduled while this arm
        // still runs and its frame stays parked until the join below releases root. Its stack
        // is caller-owned because the sibling has to be able to NAME its top.
        auto const victim = kos::thread::create(hostile_victim,
                                                const_cast<int32_t*>(verdict), "hvic", 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                reinterpret_cast<void*>(vbase), VSTK,
                                                nullptr, 0, nullptr, 0, 0, nullptr, task);
        if (not victim.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the victim");
            return;
        }
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, KOS_CAP_WAIT}};
        auto const sibling = kos::thread::create(hostile_sibling,
                                               reinterpret_cast<void*>(vbase + VSTK), "hsib",
                                               10, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0, nullptr, 0, caps, 2, 0, nullptr,
                                               task);
        if (not sibling.valid())
        {
            (void)kos_task_kill(task);
            (void)victim.join(HOSTILE_JOIN_US);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the sibling");
            return;
        }
        wait_n(1); // the scribble is COMPLETE before the victim is let go
        int const jrc = victim.join(HOSTILE_JOIN_US);
        int const rc = *verdict;
        (void)kos_task_kill(task);
        (void)sibling.join(HOSTILE_JOIN_US);
        (void)kos_handle_close(park);
        TAP_CHECK(jrc == 0);
        TAP_CHECK(rc == -KOS_EPERM);
    }

    // --- T7: THE OWNER-CARRYING, PAGE-SPLIT ACCESS SEAM (section 3.3) ----------------

    // A validated range contiguous in VIRTUAL memory whose two pages are backed by
    // NON-ADJACENT frames. NO CALLER CAN BUILD ONE HERE: a reservation's virtual address is
    // its output address, so virtual and physical adjacency are one question from userspace,
    // and the granted-range list refuses a range spanning two entries anyway. The scenario
    // is built with the map editor behind the probe; NEIGHBOUR is the sharp bit, reading the
    // frame that physically follows the low page, which is where a copy written as one
    // memcpy over a translated base spills.
    void t_split_access()
    {
        uint64_t const bits = kos_aspace_probe(KOS_ASPACE_OP_SPLIT_ACCESS, 0);
        tap::diag("split access: bits %u of %u", static_cast<unsigned>(bits),
                  static_cast<unsigned>(KOS_ASPACE_SPLIT_ALL));
        TAP_CHECK(bits == KOS_ASPACE_SPLIT_ALL);
    }

    // Two PROCESSES exchanging a message whose buffer AND whose receive-info out-pointer are
    // app static data, so both hold ONE virtual address in both spaces while naming
    // different frames, which is what makes an owner unrecoverable from an address.
    //
    // THE SENDER'S OWN RECEIVE-INFO IS THE INSTRUMENT. The kernel stores the info into the
    // RECEIVER's out-pointer while the SENDER is the running thread, so a site resolving
    // that pointer against the running space writes the sender's copy instead, and the guard
    // below is what reads that back.
    enum
    {
        PI_ADDR = 0,      // the member's own &g_pi_msg[0]
        PI_INFO_ADDR = 1, // its own &g_pi_info
        PI_FRAME = 2,     // the frame under its payload buffer
        PI_N = 3,         // what its own IPC call returned
        PI_SEEN = 4,      // payload bytes matching the pattern its role expects
        PI_BADGE = 5,
        PI_RCAP = 6,
        PI_REPLY_RC = 7, // the call arm's server only
        PI_WORDS = 8
    };
    constexpr int PI_MSG = 12;
    constexpr uint32_t PI_GUARD = 0xDEADBEEFu;
    constexpr uint32_t PI_BLK = 256;
    // VOLATILE: the question each arm asks is whether something OTHER than this side wrote
    // them, and the sender hands neither address to the call it reads them across.
    volatile char g_pi_msg[PI_MSG] = {};
    volatile kos_recv_info g_pi_info = {};

    void pi_mine(volatile uint64_t* out, char first)
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>(first + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
        out[PI_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        out[PI_INFO_ADDR] = reinterpret_cast<uintptr_t>(&g_pi_info);
        out[PI_FRAME] = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT,
                                         reinterpret_cast<uintptr_t>(&g_pi_msg[0]));
    }

    void pi_report(volatile uint64_t* out, int32_t n, char first)
    {
        out[PI_N] = static_cast<uint64_t>(static_cast<int64_t>(n));
        uint64_t seen = 0;
        for (int i = 0; i < PI_MSG; i++)
        {
            if (g_pi_msg[i] == static_cast<char>(first + i))
            {
                seen++;
            }
        }
        out[PI_SEEN] = seen;
        out[PI_BADGE] = g_pi_info.badge;
        out[PI_RCAP] = g_pi_info.reply_cap;
    }

    void pi_server(void* arg) // caps: done@1, E(WAIT)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        int32_t const n = kos_recv(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG,
                                  const_cast<kos_recv_info*>(&g_pi_info));
        pi_report(out, n, 'A');
        kos_sem_post(CH_DONE);
    }

    void pi_client(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_send(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG);
        pi_report(out, n, 'A');
        kos_sem_post(CH_DONE);
    }

    void pi_call_server(void* arg) // caps: done@1, E(WAIT)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, '\0');
        int32_t const n = kos_recv(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG,
                                  const_cast<kos_recv_info*>(&g_pi_info));
        pi_report(out, n, 'A');
        // The reply leaves the server's OWN copy of the buffer and must land in the parked
        // caller's copy, at the same number.
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('a' + i);
        }
        out[PI_REPLY_RC] = static_cast<uint64_t>(static_cast<int64_t>(
            kos_reply(g_pi_info.reply_cap, const_cast<char*>(&g_pi_msg[0]), PI_MSG)));
        kos_sem_post(CH_DONE);
    }

    void pi_call_client(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        pi_mine(out, 'A');
        int32_t const n = kos_call(2, const_cast<char*>(&g_pi_msg[0]), PI_MSG, PI_MSG);
        pi_report(out, n, 'a'); // the REPLY, in its own copy of the one address
        kos_sem_post(CH_DONE);
    }

    // Two processes, one endpoint, a report block each. The SERVER outranks the client, so
    // it is parked on the endpoint before the client's arm runs: the parked-peer arm is what
    // is under test, and a client that arrived first would exercise the sender queue.
    bool pi_two_processes(void (*server)(void*), void (*client)(void*),
                          volatile uint64_t** oa, volatile uint64_t** ob)
    {
        *oa = nullptr;
        *ob = nullptr;
        void* const ba = kos_ram_alloc(PI_BLK);
        void* const bb = kos_ram_alloc(PI_BLK);
        if (ba == nullptr or bb == nullptr)
        {
            return false;
        }
        if (kos_mem_self_grant(ba, PI_BLK, 0) != 0 or kos_mem_self_grant(bb, PI_BLK, 0) != 0)
        {
            return false;
        }
        volatile uint64_t* const sa = static_cast<volatile uint64_t*>(ba);
        volatile uint64_t* const sb = static_cast<volatile uint64_t*>(bb);
        for (int i = 0; i < PI_WORDS; i++)
        {
            sa[i] = 0;
            sb[i] = 0;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            return false;
        }
        kos_task_t ta = KOS_TASK_NONE;
        kos_task_t tb = KOS_TASK_NONE;
        if (kos_task_create(ba, PI_BLK, 0, &ta) != 0)
        {
            (void)kos_handle_close(ep);
            return false;
        }
        if (kos_task_create(bb, PI_BLK, 0, &tb) != 0)
        {
            (void)kos_task_kill(ta);
            (void)kos_handle_close(ep);
            return false;
        }
        kos_cap_grant const scaps[2] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT}};
        kos_cap_grant const ccaps[2] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        // The server is seated FIRST and the client only if it took: a client with no server
        // parks on the send queue, which root cannot release without becoming the receiver.
        bool const s_ok = kos::thread::create_caps(server, ba, "piS", 12, scaps, 2,
                                                   KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                   nullptr, ta).valid();
        bool c_ok = false;
        if (s_ok)
        {
            c_ok = kos::thread::create_caps(client, bb, "piC", 11, ccaps, 2, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, 0, nullptr, tb).valid();
        }
        if (s_ok and not c_ok)
        {
            // The server is about to park with nobody to serve it. Root sends in the
            // client's place so its post lands here and not inside a later arm.
            char pad[PI_MSG] = {};
            (void)kos_send(ep, pad, PI_MSG);
        }
        int seated = 0;
        if (s_ok)
        {
            seated++;
        }
        if (c_ok)
        {
            seated++;
        }
        wait_n(seated);
        (void)kos_task_kill(ta);
        (void)kos_task_kill(tb);
        (void)kos_handle_close(ep);
        if (seated != 2)
        {
            return false;
        }
        *oa = sa;
        *ob = sb;
        return true;
    }

    void pi_root_seed()
    {
        for (int i = 0; i < PI_MSG; i++)
        {
            g_pi_msg[i] = static_cast<char>('R' + i);
        }
        g_pi_info.badge = PI_GUARD;
        g_pi_info.reply_cap = PI_GUARD;
    }

    // Root's own copies of both, which no member's IPC may reach.
    bool pi_root_intact()
    {
        bool ok = g_pi_info.badge == PI_GUARD and g_pi_info.reply_cap == PI_GUARD;
        for (int i = 0; i < PI_MSG; i++)
        {
            ok = ok and g_pi_msg[i] == static_cast<char>('R' + i);
        }
        return ok;
    }

    void t_process_ipc_same_addr()
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        pi_root_seed();
        if (not pi_two_processes(pi_server, pi_client, &oa, &ob))
        {
            tap::skip("thread, task, arena or endpoint pool too small for 2 processes");
            return;
        }
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        uintptr_t const own_info = reinterpret_cast<uintptr_t>(&g_pi_info);
        tap::diag("same-address send: frames %u/%u, n %d/%d",
                  static_cast<unsigned>(oa[PI_FRAME]), static_cast<unsigned>(ob[PI_FRAME]),
                  static_cast<int>(static_cast<int32_t>(oa[PI_N])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        // ONE payload address and ONE out-pointer address, in both spaces and in root's.
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        TAP_CHECK(oa[PI_INFO_ADDR] == own_info and ob[PI_INFO_ADDR] == own_info);
        // Different frames under it, which is what makes the equal numbers different memory.
        TAP_CHECK(oa[PI_FRAME] != 0 and ob[PI_FRAME] != 0);
        TAP_CHECK(oa[PI_FRAME] != ob[PI_FRAME]);
        // The payload crossed, byte for byte, into the RECEIVER's copy.
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        // And so did the receive-info: a plain send, so badge 0 and no reply cap.
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) == KOS_CAP_NONE);
        // The sender's side: its own buffer unread-from and, above all, its own out-pointer
        // never written, the receiver's carrying the same number.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // The same two processes over a CALL: the request crosses on the way in, the reply cap
    // is minted into the server's own table and delivered to the server's own out-pointer,
    // and the reply crosses back into a caller parked in a third space.
    void t_process_call_reply()
    {
        volatile uint64_t* oa = nullptr;
        volatile uint64_t* ob = nullptr;
        pi_root_seed();
        if (not pi_two_processes(pi_call_server, pi_call_client, &oa, &ob))
        {
            tap::skip("thread, task, arena or endpoint pool too small for 2 processes");
            return;
        }
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_pi_msg[0]);
        tap::diag("same-address call: frames %u/%u, reply rc %d, n %d",
                  static_cast<unsigned>(oa[PI_FRAME]), static_cast<unsigned>(ob[PI_FRAME]),
                  static_cast<int>(static_cast<int32_t>(oa[PI_REPLY_RC])),
                  static_cast<int>(static_cast<int32_t>(ob[PI_N])));
        TAP_CHECK(oa[PI_ADDR] == own and ob[PI_ADDR] == own);
        TAP_CHECK(oa[PI_FRAME] != 0 and ob[PI_FRAME] != 0);
        TAP_CHECK(oa[PI_FRAME] != ob[PI_FRAME]);
        // Server side: the whole request arrived, and the reply cap was minted into the
        // server's table and delivered to the server's own out-pointer.
        TAP_CHECK(static_cast<int32_t>(oa[PI_N]) == PI_MSG);
        TAP_CHECK(oa[PI_SEEN] == PI_MSG);
        TAP_CHECK(oa[PI_BADGE] == 0);
        TAP_CHECK(static_cast<uint32_t>(oa[PI_RCAP]) != KOS_CAP_NONE
                  and static_cast<uint32_t>(oa[PI_RCAP]) != PI_GUARD);
        TAP_CHECK(static_cast<int32_t>(oa[PI_REPLY_RC]) == 0);
        // Caller side: the reply landed in ITS copy of the one address, and its own
        // out-pointer was never a target.
        TAP_CHECK(static_cast<int32_t>(ob[PI_N]) == PI_MSG);
        TAP_CHECK(ob[PI_SEEN] == PI_MSG);
        TAP_CHECK(ob[PI_BADGE] == PI_GUARD);
        TAP_CHECK(static_cast<uint32_t>(ob[PI_RCAP]) == PI_GUARD);
        TAP_CHECK(pi_root_intact());
    }

    // --- T8: THE TWO DENIALS (F5, F6) ------------------------------------------------

#if KICKOS_FAULT_ISOLATION
    constexpr uint32_t FAULT_JOIN_US = 60000;

    // The victim writes through an address ROOT reserved. That is the one address a worker can
    // be handed that is guaranteed absent from its own space: a reservation is per-task and
    // maps nothing until its own owner grants it (F10), so no other space has ever had an
    // entry for it, and root granting nothing leaves it unmapped here too.
    void fault_toucher(void* arg)
    {
        *static_cast<volatile unsigned char*>(arg) = 1u;
        // Unreachable: the write above ends this thread's whole TASK.
        kos_exit(1);
    }

    // Parked on a semaphore nothing posts, which is what makes the sibling's death evidence:
    // sem_wait has no error return to carry a reason, so only a group-scoped kill releases it.
    void fault_sibling(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
        kos_exit(1);
    }

    // F5, and the reason it is worded TASK and not thread: siblings share the address space
    // the victim was writing when it died, so containing the fault to one member would not
    // contain it. Root is the survivor and shares no space with either.
    void t_fault_kills_task()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        void* const absent = kos_ram_alloc(64);
        if (absent == nullptr)
        {
            (void)kos_handle_close(park);
            tap::skip("no reservation left to name an unmapped page with");
            return;
        }
        // READ AFTER THE RESERVATION AND BEFORE THE TASK, so the delta below covers the dying
        // task's own space and nothing else: a reservation spends pool frames that nothing ever
        // frees (F10), and root keeps this one.
        uint64_t const frames_before = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        kos_task_t task = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &task) != 0)
        {
            (void)kos_handle_close(park);
            tap::skip("no task slot");
            return;
        }
        kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
        auto const sibling = kos::thread::create(fault_sibling, nullptr, "fsib", 10,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                                /*authority=*/0, /*cap_dest=*/nullptr, task);
        if (not sibling.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the sibling");
            return;
        }
        auto const victim = kos::thread::create(fault_toucher, absent, "fvic", 10,
                                               KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                               nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                                               /*authority=*/0, /*cap_dest=*/nullptr, task);
        if (not victim.valid())
        {
            (void)kos_task_kill(task);
            (void)sibling.join(FAULT_JOIN_US);
            (void)kos_handle_close(park);
            tap::skip("pool too small for the victim");
            return;
        }
        tap::diag("faulting on 0x%lx, reserved by root and mapped in no space",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(absent)));
        // 0 AND NOT ETIMEDOUT ON BOTH. The victim's join says the fault really killed it
        // rather than resuming it, the sibling's says the kill reached the whole group, and
        // root running these lines at all is the survival half: an image that panicked
        // instead would never reach the plan's end.
        TAP_CHECK(victim.join(FAULT_JOIN_US) == 0);
        TAP_CHECK(sibling.join(FAULT_JOIN_US) == 0);
        // ROOT'S CREATOR HOLD IS DROPPED BEFORE THE POOL IS READ, and it has to be: a task
        // emptied by the fault is "emptied, not dead" while its creator can still spawn into
        // it, so its space is legitimately still alive and 36 frames of it are still out. This
        // is what the measurement below would otherwise read as a leak.
        TAP_CHECK(kos_task_kill(task) == 0);
        // T4's rule on the FAULT path, which no balance arm above reaches: they all end a space
        // through a syscall, and this is the only death that runs out of a redirect stub. The
        // group is gone and the hold is dropped, so its root, tables and stack frames are owed
        // back in full.
        uint64_t const frames_after = kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0);
        tap::diag("frame pool free %u before the task, %u after it died",
                  static_cast<unsigned>(frames_before), static_cast<unsigned>(frames_after));
        TAP_CHECK(frames_after == frames_before);
        TAP_CHECK(kos_handle_close(park) == 0);
    }
#endif

    // F6's hostile witness: an AUTHORIZED unprivileged caller self-granting a KERNEL address
    // must be REFUSED BY NAME, not merely fail to fault later. Root holds AUTH_MEMORY, so the
    // authority clause is not what answers, and every other clause on this path admits the
    // request: the size is neither 0 nor wrapping, NORMAL is honourable, and the range is not
    // already reachable. What refuses it is that the task reserved no such range.
    //
    // THE ADDRESS COMES FROM THE KERNEL. App text cannot name a kernel-half symbol under this
    // board's code model, and an address the app computed would assert the layout rather than
    // a word the kernel really owns.
    //
    // NOT WORDED AGAINST "the high half": the arena is itself high-half here, so a high
    // address inside it is admitted BY DESIGN and one outside it is refused for being
    // out-of-arena, which says nothing about the kernel.
    void t_grant_kernel_word_refused()
    {
        void* const kword = kos_guard_addr();
        if (kword == nullptr)
        {
            tap::skip("this board names no privileged-only word");
            return;
        }
        void* const mine = kos_ram_alloc(64);
        if (mine == nullptr)
        {
            tap::skip("no reservation left for the positive control");
            return;
        }
        tap::diag("self-granting the kernel word 0x%lx against own range 0x%lx",
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(kword)),
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mine)));
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
        // THE POSITIVE CONTROL, and what makes the refusal above about the ADDRESS rather
        // than about the call: the same call, on a range this task did reserve, succeeds.
        TAP_CHECK(kos_mem_self_grant(mine, 64, 0) == 0);
        // Still refused after a success on the same path, so the refusal is not a one-shot
        // state the first call left behind.
        TAP_CHECK(kos_mem_self_grant(kword, sizeof(uint32_t), 0) == -KOS_EPERM);
    }

    // --- The already-mapped short circuit, asked of the TYPE and in BOTH directions ------
    // A mapping carries its memory type here, so a re-grant naming a different one has to
    // reprogram the leaf. Answering on rights alone tells a caller that a DMA buffer is
    // ordinary memory again while the mapping stays exactly as it was.
    //
    // NOCACHE OVER NORMAL ALONE REPRODUCES THE DEFECT RATHER THAN CATCHING IT: that is the
    // one direction the typed question was ever asked in.
    void t_self_grant_retype()
    {
        constexpr uint32_t RT_BLK = 256;
        // 1 + the enum value, which is what KOS_ASPACE_OP_MEMTYPE_AT answers.
        constexpr uint64_t RT_NORMAL_AT = 1u;
        constexpr uint64_t RT_NOCACHE_AT = 2u;
        if (kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE, 1) == 0)
        {
            tap::skip("this backend honours no non-cacheable type");
            return;
        }
        void* const blk = kos_ram_alloc(RT_BLK);
        if (blk == nullptr)
        {
            tap::skip("no reservation left for the re-type block");
            return;
        }
        uintptr_t const at = reinterpret_cast<uintptr_t>(blk);
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, KOS_MEM_NOCACHE) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NOCACHE_AT);
        // THE WAY BACK, which is the transition the type exists for.
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        // Idempotent in the state it landed in, so the leg above is not a one-shot.
        TAP_CHECK(kos_mem_self_grant(blk, RT_BLK, 0) == 0);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_MEMTYPE_AT, at) == RT_NORMAL_AT);
        // Still reachable, which a re-map that broke the mapping down and failed would lose.
        volatile uint32_t* const w = static_cast<volatile uint32_t*>(blk);
        w[0] = 0xA5A5u;
        TAP_CHECK(w[0] == 0xA5A5u);
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- What a process copies its globals FROM, once root is no longer there ------------
    // Root maps the image's own data pages and the app's ctors run in root, so root is the
    // template by construction. What every later process copies has to be a SNAPSHOT of it:
    // a process that becomes the template after root is gone hands every process after it a
    // live process's mutable globals.
    volatile uint64_t g_dt_word = 0xC0FFEEull;
    constexpr uint64_t DT_A = 0xC0FFEEull;
    constexpr uint64_t DT_B = 0xBADBADull;
    enum
    {
        DT_VALUE = 0,
        DT_FRAME = 1,
        DT_WORDS = 2
    };
    void dt_reader(void* arg) // caps: done@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        out[DT_VALUE] = g_dt_word;
        out[DT_FRAME] =
            kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, reinterpret_cast<uintptr_t>(&g_dt_word));
        kos_sem_post(CH_DONE);
    }
    void t_process_data_template()
    {
        constexpr uint32_t DT_BLK = 8u * DT_WORDS;
        void* const blk = kos_ram_alloc(DT_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, DT_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        uintptr_t const own = reinterpret_cast<uintptr_t>(&g_dt_word);
        uint64_t const rootf = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, own);
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        out[DT_VALUE] = 0;
        out[DT_FRAME] = 0;
        kos_task_t t1 = KOS_TASK_NONE;
        if (kos_task_create(blk, DT_BLK, 0, &t1) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        if (not kos::thread::create_caps(dt_reader, blk, "dtA", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t1).valid())
        {
            (void)kos_task_kill(t1);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t1);
        uint64_t const first_value = out[DT_VALUE];
        uint64_t const first_frame = out[DT_FRAME];
        // THE HOME LOST, and root's own copy moved afterwards. A process created now must
        // still read the snapshot: one that mapped the image's own pages would answer root's
        // frame and root's LIVE word.
        (void)kos_aspace_probe(KOS_ASPACE_OP_DATA_HOME_FORGET, 0);
        g_dt_word = DT_B;
        out[DT_VALUE] = 0;
        out[DT_FRAME] = 0;
        kos_task_t t2 = KOS_TASK_NONE;
        if (kos_task_create(blk, DT_BLK, 0, &t2) != 0)
        {
            g_dt_word = DT_A;
            tap::skip("task pool too small for the second process");
            return;
        }
        if (not kos::thread::create_caps(dt_reader, blk, "dtB", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                         false, nullptr, 0, 0, nullptr, t2).valid())
        {
            (void)kos_task_kill(t2);
            g_dt_word = DT_A;
            tap::skip("thread pool too small for the second process");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t2);
        uint64_t const late_value = out[DT_VALUE];
        uint64_t const late_frame = out[DT_FRAME];
        g_dt_word = DT_A;
        tap::diag("data template: root frame %u, first %u/%u, after the home is lost %u/%u",
                  static_cast<unsigned>(rootf), static_cast<unsigned>(first_frame),
                  static_cast<unsigned>(first_value), static_cast<unsigned>(late_frame),
                  static_cast<unsigned>(late_value));
        TAP_CHECK(rootf != 0 and first_frame != 0 and late_frame != 0);
        TAP_CHECK(first_value == DT_A and first_frame != rootf);
        TAP_CHECK(late_value == DT_A); // the snapshot, not root's live word
        TAP_CHECK(late_frame != rootf); // a frame of its own, not the image's own page
        TAP_CHECK(kos_aspace_probe(KOS_ASPACE_OP_BALANCE, 0) == 0);
    }

    // --- libc's reentrant state and the half it lives in ---------------------------------
    // The slot array and the word libc resolves from are both the APP's, in the half a
    // translating backend switches per process. A thread whose task holds no space leaves that
    // half naming whichever process was installed last, so the switch path may write neither
    // for one: every privileged spawn resolves to the kernel domain, which carries no space,
    // and idle is in that posture on every idle window.
    //
    // BIT 0 IS THE POSITIVE CONTROL. It is counted where the guard is not, so an arm reading
    // bit 1 alone would pass on a kernel that had stopped reaching the posture at all.
    void t_reent_seating()
    {
        kos_sleep_ns(2000000ull); // an idle window inside this arm, not only in an earlier one
        uint64_t const v = kos_aspace_probe(KOS_ASPACE_OP_REENT_SEATING, 0);
        tap::diag("reent seating word %u", static_cast<unsigned>(v));
        TAP_CHECK((v & 1u) != 0);
        TAP_CHECK((v & 2u) == 0);
    }

    // --- One release per acquire, and only where one was taken ---------------------------
    // arch.h counts OUTSTANDING acquire calls, so a release beside an acquire that answered
    // null is a release of somebody else's hold. WHERE arch_aspace_release is a no-op this
    // counter is the only thing an arm can read a mispaired release off; a backend holding a
    // real window refuses one itself.
    void t_aspace_acquire_balance()
    {
        // Page zero, which no space maps: the frame-token pair's second acquire answers null
        // and its first does not.
        uint64_t const unmapped = kos_aspace_probe(KOS_ASPACE_OP_FRAME_AT, 0);
        uint64_t const bal = kos_aspace_probe(KOS_ASPACE_OP_ACQUIRE_BALANCE, 0);
        tap::diag("token for an unmapped page %u, acquire balance %u live / %u unpaired",
                  static_cast<unsigned>(unmapped), static_cast<unsigned>(bal >> 32),
                  static_cast<unsigned>(bal & 0xFFFFFFFFu));
        TAP_CHECK(unmapped == 0);
        TAP_CHECK(bal == 0);
    }

    // --- Seeding a space installed on no core costs no TLB maintenance -------------------
    // Nothing tags a translation, so every root change drops the whole low half: a space the
    // walker has never read holds neither a cached entry nor a cached absence, and its whole
    // image seed is maintenance the map editor can skip. The running space's own widening
    // cannot be, which is what the second half here reads.
    void t_map_tlbi_elided()
    {
        constexpr uint32_t TLBI_BLK = 256;
        uint64_t const before = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task or domain pool too small for one more space");
            return;
        }
        uint64_t const after = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        (void)kos_task_kill(t);
        uint32_t const issued = static_cast<uint32_t>((after >> 32) - (before >> 32));
        uint32_t const elided = static_cast<uint32_t>(after - before);
        tap::diag("image seed: %u sequences issued, %u elided", issued, elided);
        TAP_CHECK(issued == 0);
        // A seed reporting a handful mapped almost none of the image.
        TAP_CHECK(elided >= 32u);
        // The positive control, without which the arm is vacuous: an editor that had stopped
        // invalidating at all would pass the two checks above.
        void* const blk = kos_ram_alloc(TLBI_BLK);
        if (blk == nullptr)
        {
            tap::partial("no reservation left for the installed-space control");
            return;
        }
        uint64_t const pre = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        TAP_CHECK(kos_mem_self_grant(blk, TLBI_BLK, 0) == 0);
        uint64_t const post = kos_aspace_probe(KOS_ASPACE_OP_MAP_TLBI, 0);
        tap::diag("running-space widening: %u sequences issued",
                  static_cast<unsigned>((post >> 32) - (pre >> 32)));
        TAP_CHECK((post >> 32) > (pre >> 32));
    }
#endif
    void t_confused_deputy()
    {
        kos_sem_create(0, &g_cd_done);
        kos_cap_grant caps[] = {{g_cd_done, CH_FULL}};
        auto w = kos::thread::create_caps(cd_worker, nullptr, "cdwork", 10, caps, 1);
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
        // The rodata-literal positive above already covered the read path.
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
            TAP_CHECK(g_cd_bad_rc == -KOS_EFAULT); // bogus buffer rejected, never read
            TAP_CHECK(g_cd_badname_spawn == 0 and g_cd_badname_ran == 1);
        }
#endif
    }

    // --- Endpoint IPC: synchronous rendezvous send/recv ----------
    // The endpoint cap is delegated to workers at child index 2 (done@1, E@2). Workers
    // are UNPRIVILEGED so the kernel's copy into/from a parked peer runs against real
    // enforcement (the cross-domain privileged write, design section 3.1).
    char const EP_MSG[] = "hello-endpoint"; // no NUL sent
    constexpr uint8_t EP_SIGNAL_ONLY = KOS_CAP_SIGNAL;
    constexpr uint8_t EP_WAIT_ONLY = KOS_CAP_WAIT;
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rcap = 64;
        auto w = kos::thread::create_caps(ep_recv_worker, nullptr, "eprx", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull); // let the worker park in recv
        int32_t sc = kos_send(g_ep, EP_MSG, mlen);
        TAP_CHECK(sc == static_cast<int32_t>(mlen));
        wait_n(1);
        int32_t const ep_rn_a = g_ep_rn;
        TAP_CHECK(ep_rn_a == static_cast<int32_t>(mlen) and memcmp(g_ep_rbuf, EP_MSG, mlen) == 0);
        uint32_t const ep_rbadge = g_ep_rbadge;
        TAP_CHECK(ep_rbadge == 0); // badge always written on success (stage i: 0)

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        auto w2 = kos::thread::create_caps(ep_send_worker, nullptr, "eptx", 12, caps, 2,
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

        // (C) zero-length is a valid signal, not an error.
        g_ep_rn = -99; g_ep_rcap = 64;
        auto w3 = kos::thread::create_caps(ep_recv_worker, nullptr, "epz", 12, caps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w3.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, 0) == 0);
        wait_n(1);
        int32_t const ep_rn_c = g_ep_rn;
        TAP_CHECK(ep_rn_c == 0);

        // (D) truncation: a capacity below the message length.
        g_ep_rn = -99; g_ep_rcap = 4;
        auto w4 = kos::thread::create_caps(ep_recv_worker, nullptr, "eptr", 12, caps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w4.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, mlen) == 4);
        wait_n(1);
        int32_t const ep_rn_d = g_ep_rn;
        TAP_CHECK(ep_rn_d == 4 and memcmp(g_ep_rbuf, EP_MSG, 4) == 0);

        TAP_CHECK(kos_handle_close(g_ep) == 0);
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
        g_ep_wait_send_rc = static_cast<int>(kos_send(2, b, 1));
        g_ep_signal_recv_rc = static_cast<int>(kos_recv(3, b, sizeof(b), nullptr));
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_rights()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_wait_send_rc = -99; g_ep_signal_recv_rc = -99;
        // Two narrowed caps to the same endpoint: WAIT-only at index 2, SIGNAL-only at 3.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_rights_worker, nullptr, "eprt", 12, caps, 3,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        wait_n(1);
        int const ep_wait_send_rc = g_ep_wait_send_rc;
        int const ep_signal_recv_rc = g_ep_signal_recv_rc;
        TAP_CHECK(ep_wait_send_rc == -KOS_EPERM);
        TAP_CHECK(ep_signal_recv_rc == -KOS_EPERM);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- EPIPE: a parked sender is woken -KOS_EPIPE when the last WAIT holder drops it
    // A SIGNAL-only delegation does NOT bump recv_holders, so main's cap is the sole
    // WAIT holder: closing it takes recv_holders 1->0 and EPIPEs the parked sender.
    Atomic<int32_t, Order::RELAXED> g_ep_epipe_rc{-99};
    void ep_epipe_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_ep_epipe_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_epipe()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_epipe_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_epipe_worker, nullptr, "epep", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull);              // let the sender park (recv_holders == 1 == main)
        TAP_CHECK(kos_handle_close(g_ep) == 0); // last WAIT cap -> EPIPE the parked sender
        wait_n(1);
        int32_t const ep_epipe_rc = g_ep_epipe_rc;
        TAP_CHECK(ep_epipe_rc == -KOS_EPIPE);
    }

    // --- Dead endpoint (unparked): send after the last WAIT cap is gone -> -KOS_EPIPE
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
        auto w = kos::thread::create_caps(ep_dead_worker, nullptr, "epde", 12, caps, 3,
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
    // fire: the only thing missing is a parked receiver. The worker's report rides an
    // UNTIMED send on the SAME endpoint, so the report arriving proves that form still parks.
    constexpr uint32_t EP_SEND_TIMEOUT_US = 4000;
    // 20x the deadline: this sleep must still be running when the deadline fires, so the
    // margin has to absorb a timer that overruns.
    constexpr uint64_t EP_SEND_TIMEOUT_WAIT_NS = 80000000ull;
    // `entered` is the clock immediately before the timed syscall: a reading earlier than the
    // receiver's own pre-syscall reading means the caller was already parked (slow path), a
    // later one means the receiver parked first (fast path). Both stagings satisfy every rc
    // and duration assertion, so nothing else here can tell them apart.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_timed_worker, nullptr, "eptm", 12, caps, 2,
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
    // for an expiry.
    constexpr uint32_t EP_CALL_TIMEOUT_US = 4000;
    // 20x, for the same reason as EP_SEND_TIMEOUT_WAIT_NS.
    constexpr uint64_t EP_CALL_TIMEOUT_WAIT_NS = 80000000ull;
    constexpr uint64_t EP_CALL_SETTLE_NS = 3000000ull; // long enough for the caller to park
    // The reply-wait arm needs the OPPOSITE ordering: the server must pop the caller BEFORE
    // its deadline fires, so the settle sleep has to finish inside the deadline. At a 1x
    // margin the caller times out first, the server's recv returns the report instead of the
    // request, and the arm leaks its reply cap into the census cap_child_width reads.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_call_pending_worker, nullptr, "cltp", 12, caps, 2,
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
    // The caller parks on send_waiters first (main sleeps instead of recv'ing), and main's
    // info-bearing recv then MIGRATES it onto main's reply_waiters. That migration is a
    // park-to-park move and not an unpark, so the deadline armed once at the call must
    // survive it: were the cancel back in wq_pop_highest, this caller would park forever and
    // the arm would HANG rather than fail. Main never replies.
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
        auto w = kos::thread::create_caps(ep_call_reply_worker, nullptr, "cltr", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        // Root is the lowest-priority thread, so once this sleep blocks root the caller
        // runs to its own park before root can resume.
        kos_sleep_ns(EP_CALL_SETTLE_NS); // the caller is now parked as CALL_SEND_WAIT
        char req[8];
        // A TIMED recv here, generously bounded: the caller is already parked, so this pops
        // it without arming a deadline, and the surviving opts.timeout_us shows the kernel
        // wrote the NESTED kos_recv_info and left the input word alone.
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
        // THE staging witness: the caller's pre-call reading precedes main's pre-recv
        // reading, so the caller was already parked and the recv POPPED it. A fast-path
        // staging inverts this and fails here rather than passing on the untested path.
        TAP_CHECK(r.entered < before_recv);
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // the deadline crossed the handoff and fired
        TAP_CHECK(r.waited_us >= EP_CALL_REPLY_TIMEOUT_US);
        // The cap outlives the caller by design, so closing it is still the server's job and
        // must succeed with nobody left to wake.
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
        auto w = kos::thread::create_caps(ep_reply_stale_worker, nullptr, "rpst", 12, caps, 2,
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
        // main's reading, so main was parked in the recv when the call landed. That is the
        // FAST path, and a slow-path staging fails here instead of duplicating the other arm.
        TAP_CHECK(r.entered > before_recv);
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // expired on the reply park, not on send_waiters
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Reply on an abandoned cap once its sequence has come round again ---------------
    // This arm reaches PAST the call_state test that reply_stale_caller stops at. The caller
    // times out, leaving a live one-shot cap in a server's table, then runs further calls
    // until its call_seq low byte comes back to the one packed in that cap: the abandoned cap
    // then resolves through index, generation, BLOCKED, CALL_REPLY_WAIT and sequence.
    //
    // What stops it is that the caller is parked on the SECOND server's reply_waiters, so
    // the abandoned holder's unlink finds nothing of its own. Without that the reply copies
    // bytes into a buffer belonging to a transaction it has no part in.
    //
    // A SECOND server, not a second call to the same one: the holder of the abandoned cap is
    // already at KICKOS_CAP_REPLY_MAX and can mint no other.
    //
    // Main learns that the second server has taken the aliasing call by RENDEZVOUS on a
    // second endpoint. That endpoint is not a convenience: on one endpoint main's recv could
    // pop a loop call instead of the token, mint a reply cap for it and strand the caller.
    char const SR_GOOD[] = "OK!!";
    char const SR_BAD[] = "BAD!";
    // The abandoned call left call_seq at A and the timeout unwind rolled it to A+1, so the
    // caller's k-th further call runs at A+1+k and the packed low byte (A) comes round again
    // at k == 255. The loops below are sized so the LAST call is exactly that one: anything
    // shorter and the sequence test refuses the cap before this arm's guard is consulted.
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
        auto cl = kos::thread::create_caps(sr_caller, nullptr, "srC", 12, ccaps, 3);
        auto s2 = kos::thread::create_caps(sr_second_server, nullptr, "srS", 10, scaps, 3);
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
    // Runs in main with NO worker: nothing arrives, so the deadline alone is what expires
    // and no cross-domain copy is involved.
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
    // arm alone. The EFAULT half needs a pointer the caller does not own, which a privileged
    // caller can never present: it lives in t_endpoint_bound.
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
        // The timed call packs both lengths into one argument word and SATURATES rather than
        // masking. A masked 512 would arrive as 0 and become a silent zero-length call; the
        // saturated value is still above KOS_EP_MSG_MAX, so the F4 refusal survives packing.
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
    // WAIT_EP_SEND branch: a lost boost changes no return code anywhere else.
    //
    // Staging, in units of mtx_time_unit(): the server takes main's plain send first, which
    // seats it as the endpoint's conventional server. It then busy-spins for the rest of the
    // arm, so it is never in recv and the caller must park on send_waiters. The caller (high)
    // wakes mid-spin and calls with a deadline; the spoiler (medium) wakes after that and is
    // held off only by the boost. 'u' before 'm' is the boost holding; 'm' before 'z' is the
    // unwind dropping it. Without the revert the server stays pinned and 'z' comes first.
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
        auto sv = kos::thread::create_caps(ctr_server, nullptr, "ctrS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(ctr_caller, nullptr, "ctrC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(ctr_spoiler, nullptr, "ctrM", 12, mcaps, 2);
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
    // A high caller's slow-path kos_call boosts the low server it targets (D2). An INFO-LESS
    // recv cannot host the call, so recv rejects the caller (-KOS_ENOSYS) and MUST revert
    // that boost: 'u' before 'm' while boosted, 'm' before 'z' after the revert. Without the
    // revert the server stays pinned above the spoiler and 'z' precedes 'm'.
    //
    // Staged by one semaphore, not by sleeps: the server posts once its recv has seated it,
    // the caller re-posts for the spoiler just before calling.
    //
    // BOTH plain sends come from the filler and both are staged: the first is the only sender
    // recv#1 can see, the second is released only once the caller's call has bounced. recv#2
    // therefore finds the caller ALONE, rejects it and PARKS, which is the wake-inside-the-scan
    // path. Root must send nothing, or which recv ate which depends on whether a tick lands
    // inside root's spawn run.
    Atomic<int32_t, Order::RELAXED> g_ci_rc{-99};
    kos_cap_t g_ci_bounced = KOS_CAP_NONE; // caller -> filler: the call has bounced
    void ci_server(void*) // caps: done@1, lock@2, E(WAIT)@3, stage@4
    {
        char buf[16];
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#1 (info-less): eats the filler's 1st send; ep->server = us
        log_put('a');
        // Only now is the caller released: the D2 boost is conditional on ep->server, which
        // recv#1 above is what seats, so a caller that ran first would boost nothing.
        kos_sem_post(4);
        mtx_spin(g_call_unit * 4);              // the caller D2-boosted us inside this
        log_put('u');
        kos_recv(3, buf, sizeof(buf), nullptr); // recv#2: reject the parked call (deflate us), then park
        log_put('z');                          // reached at base prio: spoiler ran first IFF we reverted
        kos_sem_post(CH_DONE);
    }
    void ci_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, stage@4, bounced@5
    {
        char buf[8] = {0};
        kos_sem_wait(4);                       // the server is seated
        kos_sem_post(4);                       // the spoiler is ready from here, and we outrank it
        g_ci_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        // The reject and the park are one masked window, so the bounce returning here is
        // the proof recv#2 is already on recv_waiters.
        kos_sem_post(5);
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
    void ci_filler(void*) // caps: done@1, lock@2, E(SIGNAL)@3, bounced@4
    {
        char b[4] = {0};
        kos_send(3, b, 4);                     // the only sender recv#1 can see
        kos_sem_wait(4);
        kos_send(3, b, 4);                     // wakes the parked recv#2
        kos_sem_post(CH_DONE);
    }
    void t_call_infoless_revert()
    {
        // Ask the pool BEFORE spawning anything: the four workers are mutually dependent, so
        // a partial set cannot be drained. Guarding after the spawns HANGS, it does not skip.
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
        // Its own semaphore, not a third post on g_gate: the filler outranks the spoiler,
        // so a shared gate would hand the caller's pre-call post to the filler and its
        // second send would be parked before recv#2 ever ran.
        TAP_CHECK(kos_sem_create(0, &g_ci_bounced) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {g_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_gate, CH_FULL}, {g_ci_bounced, CH_FULL}};
        kos_cap_grant fcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_ci_bounced, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL}};
        auto sv = kos::thread::create_caps(ci_server, nullptr, "ciS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(ci_caller, nullptr, "ciC", 20, ccaps, 5);
        auto sp = kos::thread::create_caps(ci_spoiler, nullptr, "ciM", 12, mcaps, 3);
        // ABOVE the spoiler: its second send is what wakes the parked server, and that wake
        // must land while the spoiler is still only ready. Below the spoiler, 'm' is logged
        // before the server is woken at all and the REVERT check below passes either way.
        auto fl = kos::thread::create_caps(ci_filler, nullptr, "ciF", 14, fcaps, 4);
        // The probe above just held four slots and four stacks, so a failure now is a pool
        // bug, not a small board.
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid() and fl.valid());
        wait_n(4);
        TAP_CHECK(kos_handle_close(g_ci_bounced) == 0);
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
        auto sv = kos::thread::create_caps(cc_server, nullptr, "ccS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(cc_caller, nullptr, "ccC", 20, ccaps, 3);
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
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};

        // (A) fastpath: server parks in recv first, then the caller calls.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto sv = kos::thread::create_caps(echo_server, nullptr, "echS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath)
            cl = kos::thread::create_caps(echo_caller, nullptr, "echC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            // cl is spawned only after sv succeeds, so a skip means nothing spawned or a lone
            // server parked in recv. Neither can be drained: close and skip.
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        int32_t const echo_rc_a = g_echo_rc;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(echo_rc_a == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);

        // (B) slowpath: caller parks in SEND_WAIT first, server recvs later.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto cl2 = kos::thread::create_caps(echo_caller, nullptr, "echC2", 12, ccaps, 2);
        kos::thread::Handle sv2;
        if (cl2.valid())
        {
            kos_sleep_ns(3000000ull); // let the caller park in SEND_WAIT (slowpath)
            sv2 = kos::thread::create_caps(echo_server, nullptr, "echS2", 10, scaps, 2);
        }
        if (not cl2.valid() or not sv2.valid())
        {
            // The caller (spawned first) may be parked in SEND_WAIT: close FIRST so it is
            // EPIPE'd and posts, THEN drain it.
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

#if defined(KICKOS_ENABLE_SELFTEST) // kos_ipc_fast_taken is a selftest-only syscall
    // --- Call/reply: the trap-handler register fastpath ------------------------------
    // The fastpath and the buffer form answer a caller IDENTICALLY, so no return value can
    // tell them apart and kos_ipc_fast_taken is the only witness that the arm under test
    // ran at all. Every assertion below pairs a content check with a counter delta.
    //
    // The peers run at EQUAL priority: the fastpath refuses when the caller outranks the
    // server, so a caller above its server would measure the refusal and pass with the
    // fastpath never once taken.
    //
    // The reply travels back through the CALLER'S SAVED TRAP FRAME rather than its buffer,
    // so a wrong frame offset surfaces as wrong bytes and not as a fault. That is why the
    // server transforms the request instead of echoing it: a reply that is byte-identical
    // to the request cannot distinguish a correct copy from a buffer the kernel left alone.
    constexpr unsigned char FP_XOR = 0xA5;
    constexpr size_t FP_MAX = 20; // KOS_CALL_REG_BYTES
    // The content checks hold either way, so the arm also covers the buffer form where the
    // backend has no fastpath and the counter cannot move.
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    constexpr uint32_t FP_EXPECT_TAKEN = 1;
#else
    constexpr uint32_t FP_EXPECT_TAKEN = 0;
#endif
    Atomic<int32_t, Order::RELAXED> g_fp_reqn{-99};
    Atomic<int32_t, Order::RELAXED> g_fp_rc{-99};
    unsigned char g_fp_rpl[FP_MAX];
    size_t g_fp_send_len = 0;
    size_t g_fp_recv_cap = 0;

    void fp_server(void*) // caps: done@1, E(WAIT)@2
    {
        unsigned char buf[64];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        int32_t const n = kos_recv(2, buf, sizeof(buf), &info);
        g_fp_reqn = n;
        if (info.reply_cap != KOS_CAP_NONE and n >= 0)
        {
            for (int32_t i = 0; i < n; i++)
            {
                buf[i] = static_cast<unsigned char>(buf[i] ^ FP_XOR);
            }
            kos_reply(info.reply_cap, buf, static_cast<size_t>(n));
        }
        kos_sem_post(CH_DONE);
    }
    void fp_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        unsigned char buf[64];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i + 1); // no zero byte: a cleared buffer shows
        }
        g_fp_rc = kos_call(2, buf, g_fp_send_len, g_fp_recv_cap);
        int32_t const rc = g_fp_rc;
        if (rc > 0)
        {
            size_t k = static_cast<size_t>(rc);
            if (k > FP_MAX)
            {
                k = FP_MAX;
            }
            memcpy(g_fp_rpl, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    // Runs one call at the given lengths and reports how far the fastpath counter moved.
    // Takes no TAP_CHECK: that returns from its enclosing function, which here would
    // abandon the peers mid-transaction and strand the caller's semaphore posts.
    enum FpStatus
    {
        FP_RAN = 0,
        FP_NO_POOL = 1,  // the pool could not seat both peers
        FP_EP_REFUSED = 2 // endpoint create or close refused
    };
    int fp_run(size_t send_len, size_t recv_cap, uint32_t* taken_delta)
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};
        g_fp_send_len = send_len;
        g_fp_recv_cap = recv_cap;
        g_fp_reqn = -99;
        g_fp_rc = -99;
        memset(g_fp_rpl, 0, sizeof(g_fp_rpl));
        if (kos_endpoint_create(&g_ep) != 0)
        {
            return FP_EP_REFUSED;
        }
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        uint32_t const before = kos_ipc_fast_taken();
        auto sv = kos::thread::create_caps(fp_server, nullptr, "fpS", 11, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // the server must be PARKED in recv before the call
            cl = kos::thread::create_caps(fp_caller, nullptr, "fpC", 11, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep);
            return FP_NO_POOL;
        }
        wait_n(2);
        *taken_delta = kos_ipc_fast_taken() - before;
        if (kos_handle_close(g_ep) != 0)
        {
            return FP_EP_REFUSED;
        }
        return FP_RAN;
    }
    void t_call_reg_fastpath()
    {
        uint32_t taken = 0;

        // (A) both lengths inside the register budget: the fastpath runs.
        int st = fp_run(8, 8, &taken);
        if (st == FP_NO_POOL)
        {
            tap::skip("pool too small for 2 threads");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_a = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_a == 8);
        bool ok_a = true;
        for (size_t i = 0; i < 8; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_a = false;
            }
        }
        TAP_CHECK(ok_a); // every reply byte, through the caller's saved trap frame
        TAP_CHECK(taken == FP_EXPECT_TAKEN);

        // (B) the boundary, exactly KOS_CALL_REG_BYTES each way.
        st = fp_run(FP_MAX, FP_MAX, &taken);
        if (st == FP_NO_POOL)
        {
            tap::partial("boundary half not run (pool too small)");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_b = g_fp_rc;
        TAP_CHECK(g_fp_reqn == static_cast<int32_t>(FP_MAX));
        TAP_CHECK(rc_b == static_cast<int32_t>(FP_MAX));
        bool ok_b = true;
        for (size_t i = 0; i < FP_MAX; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_b = false;
            }
        }
        TAP_CHECK(ok_b);
        TAP_CHECK(taken == FP_EXPECT_TAKEN);

        // (C) a reply capacity ABOVE the budget keeps the buffer form, and the counter is
        // what says so: the bytes alone would look the same either way.
        st = fp_run(8, 64, &taken);
        if (st == FP_NO_POOL)
        {
            tap::partial("buffer-form half not run (pool too small)");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_c = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_c == 8);
        bool ok_c = true;
        for (size_t i = 0; i < 8; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_c = false;
            }
        }
        TAP_CHECK(ok_c);
        TAP_CHECK(taken == 0);
    }
#endif // KICKOS_ENABLE_SELFTEST (register-fastpath witness)

    // --- Call/reply: root calls like any other thread --------------------------------
    // The orchestrator IS root, and root holds an ordinary thread-pool slot, so a reply
    // capability can name it. Both dispatch sites run against one spawned echo server: (A)
    // the server parked in recv before root calls, (B) root parked in SEND_WAIT first. The
    // server must exist BEFORE the call either way, or the call is -KOS_EPIPE.
    //
    // The server runs at KICKOS_PRIO_MIN, one BELOW root, and both halves rest on that:
    // above root it can park in recv between the spawn and the call, making half (B) a
    // second fastpath run that says so nowhere, and every donation site is `>`-guarded, so
    // only a server root outranks makes root DONATE. The price is the drain sleep in each
    // half, since the server is still READY with only its exit left when root resumes.
    void t_call_from_root()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        char buf[16];

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        auto sv = kos::thread::create_caps(echo_server, nullptr, "rtS", 1, scaps, 2);
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
        auto sv2 = kos::thread::create_caps(echo_server, nullptr, "rtS2", 1, scaps, 2);
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
        int32_t n = kos_recv(2, buf, sizeof(buf), &info);
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
        auto sv = kos::thread::create_caps(trunc_server, nullptr, "trS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::create_caps(trunc_caller, nullptr, "trC", 12, ccaps, 2);
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
        TAP_CHECK(trunc_reqn == 3 and memcmp(g_trunc_reqbuf, "ABC", 3) == 0);
        TAP_CHECK(trunc_rc == 3 and memcmp(g_trunc_rplbuf, "123", 3) == 0);
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
        auto sv = kos::thread::create_caps(dr_server, nullptr, "drS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::create_caps(dr_caller, nullptr, "drC", 12, ccaps, 2);
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
        TAP_CHECK(dr_callrc == 2);
        TAP_CHECK(dr_second == -KOS_EBADF);
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
        auto sv = kos::thread::create_caps(sd_server, nullptr, "sdS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath call)
            cl = kos::thread::create_caps(sd_caller, nullptr, "sdC", 12, ccaps, 2);
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
        TAP_CHECK(sd_callrc == -KOS_EPIPE);
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
        auto cl = kos::thread::create_caps(pp_caller, nullptr, "ppC", 12, ccaps, 2);
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
    // the transaction) cannot preempt: the reply reaches the caller ('c') before the spoiler
    // runs ('m'). Without donation the spoiler preempts the low server and 'm' precedes 'c'.
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
        auto sv = kos::thread::create_caps(don_server, nullptr, "dnS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(don_caller, nullptr, "dnC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(don_spoiler, nullptr, "dnM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            // Drain whoever spawned: each posts g_done (the caller drives the server through
            // its reply, the spoiler is timed).
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
        TAP_CHECK(don_rc == 5 and memcmp(g_don_rpl, "pong!", 5) == 0);
        TAP_CHECK(nth('r', 1) < nth('c', 1)); // reply delivered: the caller ran after the server replied
        TAP_CHECK(nth('c', 1) < nth('m', 1)); // DONATION: the reply reached the caller before the spoiler ran
    }

    // --- Call/reply (D3): a donation must survive an UNRELATED recompute ----------
    // These cover the boost being KEPT: the server runs an unrelated mutex_unlock inside the
    // transaction, which funnels through thread_effective_prio, and the funnel must re-derive
    // the live donation instead of dropping the server back to base. Two donor kinds, each
    // holding the boost ALONE, over three arms (the hold kind is reached from both mints):
    //   hold:    a live reply cap (the caller is parked in REPLY_WAIT)
    //   pending: a caller parked in SEND_WAIT on an endpoint this thread serves
    // If the recompute deflates the server to 8, the already-awake spoiler (12) preempts it
    // and 'm' precedes 'r'.
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
        auto sv = kos::thread::create_caps(dh_server, nullptr, "dhS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(dh_caller, nullptr, "dhC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(dh_spoiler, nullptr, "dhM", 12, mcaps, 2);
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
        // The whole arm: the unlock's recompute did NOT deflate the server.
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same again for the OTHER mint site: the caller parks in SEND_WAIT first and the
    // server's recv pops it, so the reply cap is minted from the server's own syscall. The
    // two sites link the donor independently.
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
        auto sv = kos::thread::create_caps(ds_server, nullptr, "dsS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(ds_caller, nullptr, "dsC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(ds_spoiler, nullptr, "dsM", 12, mcaps, 2);
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
        auto sv = kos::thread::create_caps(dp_server, nullptr, "dpS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(dp_caller, nullptr, "dpC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(dh_spoiler, nullptr, "dpM", 12, mcaps, 2);
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
        TAP_CHECK(dh_rc == 2);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // --- Bus service: per-device slot profiles ---------------------------
    // Gated for FLASH, not for a syscall: the mock backend plus serve_one cost ~1.3 KiB, and
    // the non-selftest bluepill-c8 image has ~1.3 KiB of its 64 KiB left. Every CI gate sets
    // the flag.
#if defined(KICKOS_ENABLE_SELFTEST)
    // A controller has a single live profile register set, so kickos::spi::serve_one keeps
    // one device HANDLE per kos_bus_req.device. The backend here is spi_mock.cc, which fills
    // the buffer with the word size of the handle it was given, so a transfer on slot 0 must
    // read back slot 0's word size even after slot 1 was opened.
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
        auto cl = kos::thread::create_caps(slot_client, nullptr, "slot", 12, ccaps, 2,
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
    // serve_one touches only the shared block, never a register, so the whole request/reply
    // surface is testable with NO device at all. MAIN is the server, so the client is the
    // one thread spawned.
    //
    // The TX doorbell is structurally NOT coverable here: serve_one rings kos_irq_notify on
    // child cap index 2, which in root's own table is the authority slot, so a root-as-server
    // shape cannot host a line cap there.
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
            r.stats_tx = static_cast<int>(kos_counter_load(&s.tx_bytes));
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
            tap::skip("arena cannot spare the 1 KiB UART block, board too small");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, sizeof(kickos::uart::Shared), 0) == 0);
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(blk);
        kickos::uart::shared_init(sh);
        // Stand in for the IRQ thread: put four bytes in the RX ring so the READ below
        // has something to return. In the real driver only the IRQ thread pushes here.
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        TAP_CHECK(kos_byte_ring_push(&sh->rx, rx, 4) == 4);

        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::create_caps(uart_client, nullptr, "uartcl", 12, ccaps, 2,
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

        TAP_CHECK(got.wr == 4);
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
    // --- Bound-check: a recv/send pointer outside the caller's regions -> -KOS_EFAULT
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};
        auto w = kos::thread::create_caps(ep_bound_worker, nullptr, "epbn", 12, caps, 2,
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
    // Root and an UNPRIVILEGED worker in a DIFFERENT memory domain rendezvous both ways: the
    // arriving side's kernel copy lands in the parked peer's domain, not the arriver's loaded
    // regions. The worker's payload buffer lives in its own granted domain region, and the
    // clean endpoint free at the end is what validates the delegation accounting.
    kos_cap_t g_xd_done = KOS_CAP_NONE; // PRIVATE completion sem: the worker posts it at CH_DONE, not the shared g_done
    // A ROUND TRIP AND NOT A PAIR OF REPORTS. The worker carries a memory grant, so it is a
    // task and an address space of its OWN, and an app global it writes is its own copy of
    // one (docs/design-m6-mmu.md section 3.4). Its verdict therefore comes back the same way
    // the payload went out, over the endpoint, and root reads both ends in its own space.
    void xd_worker(void* arg) // caps: done@1, E(FULL)@2; arg = domain buffer
    {
        char* b = static_cast<char*>(arg);
        int32_t const n = kos_recv(2, b, 8, nullptr);
        int32_t verdict = n;
        for (int i = 0; i < 8; i++)
        {
            if (b[i] != static_cast<char>('a' + i))
            {
                verdict = -1;
            }
        }
        (void)kos_send(2, &verdict, sizeof(verdict));
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_crossdomain()
    {
        void* wbuf = kos_ram_alloc(256);
        if (wbuf == nullptr)
        {
            tap::skip("arena cannot spare a domain region");
            return;
        }
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_sem_create(0, &g_xd_done); // PRIVATE: never satisfies another test's wait_n(g_done)
        kos_cap_grant wcaps[] = {{g_xd_done, CH_FULL}, {g_ep, CH_FULL}};
        auto w = kos::thread::create_caps(xd_worker, wbuf, "xdW", 12, wcaps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, wbuf, 256);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_handle_close(g_ep);
            kos_sem_destroy(g_xd_done);
            return;
        }
        char out[8];
        for (int i = 0; i < 8; i++)
        {
            out[i] = static_cast<char>('a' + i);
        }
        int32_t const sent = kos_send(g_ep, out, 8);
        int32_t verdict = -99;
        int32_t const got = kos_recv(g_ep, &verdict, sizeof(verdict), nullptr);
        kos_sem_wait(g_xd_done); // this test's own completion sem, not the shared g_done
        // RECLAIMED BEFORE THE VERDICT: a TAP_CHECK returns on the spot, and an endpoint left
        // behind here starves every later arm that needs one.
        int const closed = kos_handle_close(g_ep); // both delegated caps torn down -> freed
        kos_sem_destroy(g_xd_done);
        TAP_CHECK(sent == 8);
        TAP_CHECK(got == static_cast<int32_t>(sizeof(verdict)));
        // 8 is the worker's own recv count, so this says the byte-exact payload crossed the
        // address-space boundary and the verdict crossed it back.
        TAP_CHECK(verdict == 8);
        TAP_CHECK(closed == 0);
    }

    // One own-create of whichever object type this board still has a pool slot for, reporting
    // which pool answered. A create allocates its OBJECT before installing the cap, so a pool
    // that empties on the same create that fills the table returns the pool refusal and says
    // nothing about the table; the mutex fallback gets past that. -KOS_EMFILE out of here
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
        // The reserved plane is never threaded onto the run's free list, so an own create
        // cannot pop a well-known slot (0 = console default, 1..FIRST_DYNAMIC-1 =
        // board/service delegation). Delegation seats an explicit index and so cannot catch a
        // free list built from a lower index; only an OWN create can.
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
        // <kickos/sys.h>, which puts NO byte on the wire in either posture.
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
        // KCAP_CHUNK_TARGET. A table no wider than this compiles the FLAT decode and has no
        // segmented slot to reach, so a hardcoded mirror of the granule would make this arm
        // claim the segmented path on a board that never compiled it.
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

        // USABLE, not merely numbered: reaching the object is the only proof the directory
        // index and the in-chunk offset recombined onto the entry the install wrote.
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
        // then leaves the released slot as the free list's ONLY node, forcing the next install
        // back onto that index; with any slot still free the mint lands elsewhere (a release
        // goes to the TAIL, cap.h) and the cap-gen test stays unreachable.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto w = kos::thread::create_caps(width_child, nullptr, "cw", 10, caps, 2);
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
        auto sv = kos::thread::create_caps(rb_server, units(server_delay), "rbS", 8, scaps, 3);
        auto ca = kos::thread::create_caps(rb_caller_a, units(1), "rbA", 20, ccaps, 3);
        auto cb = kos::thread::create_caps(rb_caller_b, units(b_delay), "rbB", 12, ccaps, 3);
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
        auto sv = kos::thread::create_caps(rp_server, units(1), "rpS", 8, scaps, 3);
        auto ca = kos::thread::create_caps(rp_caller, units(1), "rpA", 20, ccaps, 3);
        auto cb = kos::thread::create_caps(rp_caller, units(5), "rpB", 12, ccaps, 3);
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
        auto s1 = kos::thread::create_caps(rr_dying_server, nullptr, "rrD", 8, scaps, 3);
        auto c1 = kos::thread::create_caps(rp_caller, units(2), "rr1", 12, ccaps, 3);
        TAP_CHECK(s1.valid() and c1.valid());
        wait_n(2);
        TAP_CHECK(count('d') == 1 and count('P') == 1); // died holding a live reply cap

        // Both slots are EXITED now, so these two reclaim them.
        auto s2 = kos::thread::create_caps(rp_server, nullptr, "rrS", 8, scaps, 3);
        auto c2 = kos::thread::create_caps(rp_caller, units(2), "rr2", 12, ccaps, 3);
        TAP_CHECK(s2.valid() and c2.valid());
        char plain[4] = {0};
        kos_sleep_ns(g_call_unit * 6);
        kos_send(g_ep, plain, 4);
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('K') == 1 and count('B') == 1); // the next occupant admitted its first
    }

    // --- console_publish needs AUTH_CONSOLE; a bad cap is rejected with no side effect ---
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console. The rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1);
        kos_sem_post(CH_DONE);
    }
    void t_console_publish()
    {
        // From root, which holds AUTH_CONSOLE: a bad/stale cap is rejected before the
        // deinit/flip, so console ownership stays as the board's service list set it. This
        // test never publishes anything itself.
        // The -KOS_EBADF assertions are exact: the AUTH_CONSOLE gate runs before the cap
        // resolve, so a root that lost the bit answers -KOS_EPERM and this test fails.
        TAP_CHECK(kos_console_publish(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_console_publish(0x7fffffff) == -KOS_EBADF);
        // Unprivileged child: the privileged-only gate rejects it.
        g_pub_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(pub_denied_worker, nullptr, "pubden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_pub_rc == -KOS_EPERM);
    }

    // --- shutdown is privileged-only: an unprivileged thread cannot end the system ----
    int g_shutdown_rc = -99;
    void shutdown_denied_worker(void*) // caps: done@1
    {
        // Status 0 on purpose: were the call ever granted, the run ends here with a clean
        // exit status, which the gate sees as a truncated TAP stream.
        g_shutdown_rc = kos_shutdown(0);
        kos_sem_post(CH_DONE);
    }
    void t_shutdown_denied()
    {
        g_shutdown_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(shutdown_denied_worker, nullptr, "sdden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_shutdown_rc == -KOS_EPERM);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- reboot-to-bootloader is privileged-only: the REFUSAL arm only -------------
    // The REFUSAL arm only: on picopi/pizero2350/teensy41 a root kos_reboot really reboots
    // the board mid-run and truncates the TAP stream, so the privileged arm lives in
    // apps/rebootdemo.
    int g_reboot_rc = -99;
    void reboot_denied_worker(void*) // caps: done@1
    {
        g_reboot_rc = kos_reboot();
        kos_sem_post(CH_DONE);
    }
    void t_reboot_denied()
    {
        g_reboot_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(reboot_denied_worker, nullptr, "rbden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_reboot_rc == -KOS_EPERM);
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(wrbuf_worker, nullptr, "wrGlob", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_wrbuf_rc == -KOS_EBADF); // not -KOS_EFAULT: the global was writable
    }

    // --- the READ twin, and it aims at app RODATA rather than app .data ---------------
    // Between them the two arms cover BOTH extents of the app's own window: the writable
    // one above and the read-execute one here. On a translating backend neither is a region
    // any grant path recorded, so the only thing that admits either is the granted-range
    // list the address space was seeded with (docs/design-m6-mmu.md section 3.3). Remove
    // that seeding and both go red.
    //
    // send validates its buffer before resolving the cap, exactly as recv does, so the
    // deliberately invalid cap separates the two answers the same way.
    //
    // RUN FROM ROOT AND NOT FROM A WORKER, which is what makes this arm's own answer the
    // thing that fails. Root is unprivileged, so the same admission applies; a worker is a
    // sibling in root's task holding root's space, so it would add nothing here while making
    // a refusal present as a failed spawn.
    char const g_rdbuf[16] = "readable-global";
    void t_readable_global()
    {
        TAP_CHECK(kos_send(0x7fffffff, g_rdbuf, sizeof(g_rdbuf)) == -KOS_EBADF);
    }

    // --- The authority capability: the non-privileged arm of the authority gates ------
    // Each authority gate is `privileged OR holds this AUTH_* bit`; root is unprivileged
    // and seated with CAP_AUTH_ALL, so the rest of the suite only exercises the
    // every-bit-held arm.
    //
    // The child is UNPRIVILEGED and holds AUTH_PINMUX and nothing else, so exactly one gate
    // must accept it and the rest must refuse. Acceptance reads as "not -KOS_EPERM": a gate
    // that lets the call through returns its OWN answer instead (-KOS_ENOSYS on a
    // declining-fallback target like the sim, -KOS_EINVAL where a chip owns the block).
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
        // rather than one lump. Safe to call only BECAUSE the child lacks AUTH_SYSTEM: were
        // it granted, the run ends here with a clean status, which the harness sees as a
        // truncated TAP stream.
        g_auth_shutdown = kos_shutdown(0);
        // Three spawn probes off ONE params struct, all refused before a pool slot is
        // claimed, so their codes are deterministic even on a full pool.
        //
        // g_auth_kid and g_auth_two are globals on purpose: thread_create_call reads the params
        // struct and the grant array through user_readable_ok, so a caller may keep either
        // in static data, and that is what this covers.
        kos_thread_params& kid = g_auth_kid;
        kos_thread_t kidh = KOS_THREAD_NONE;
        kid.entry = auth_noop;
        kid.prio = 9;
        // Narrow-only, the same rule a cap_grant mask obeys: holding AUTH_PINMUX does not
        // let it seat AUTH_SYSTEM on a child.
        kid.authority = KOS_AUTH_SYSTEM;
        g_auth_regrant = kos_thread_create(&kid, &kidh);
        // cap_count is bounded by KICKOS_MAX_SPAWN_GRANTS, which is the spawn stager's
        // caller-stack budget and NOT the child table's ceiling. 255 exceeds it on every
        // board. Refused on the COUNT, before the array is read: g_auth_two is two
        // entries long, so a bound checked after the read would fault here.
        g_auth_two[0] = {CH_DONE, CH_FULL};
        g_auth_two[1] = {CH_DONE, CH_FULL};
        kid.caps = g_auth_two;
        kid.cap_count = 255;
        kid.authority = KOS_AUTH_PINMUX;
        g_auth_toomany = kos_thread_create(&kid, &kidh);
        // A bit no gate reads is refused, not masked off. It has to come from ABOVE the
        // six defined authorities: the authority word has its own numbering, separate
        // from the shared rights byte, so bits 0..5 are all real authorities and an
        // object right like KOS_CAP_WAIT is not a distinguishable wrong value here.
        kid.cap_count = 1;
        kid.authority = 1u << 6;
        g_auth_badbits = kos_thread_create(&kid, &kidh);
        // The three probes above are refused before the delegation loop, so none of them
        // reads g_auth_two. Covering the static grant ARRAY needs a probe that gets that
        // far: an unresolvable source_cap is refused -KOS_EBADF from inside the loop,
        // reachable only once the array is admitted. Refused before a slot is claimed.
        g_auth_two[0] = {0x7fffffff, CH_FULL};
        kid.authority = 0;
        g_auth_capsarr = kos_thread_create(&kid, &kidh);
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(auth_worker, nullptr, "authW", 10, caps, 1,
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
    // Refusals only; the complementary PASS arm belongs to an enforcing board.
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
        if (p != nullptr and kos_mem_self_grant(p, 1, 0) == 0)
        {
            g_pe_ram = kos_periph_enable(reinterpret_cast<uintptr_t>(p));
            g_pe_ram_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_periph_enable_unheld()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(periph_enable_worker, nullptr, "peW", 10, caps, 1,
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
        // arena still has one past kmain's two boot stacks and the per-slot default stacks
        // the thread pool bump-allocates.
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
    // ONE byte of .bss carries every arm's verdict, not a result word per arm: this file's
    // static RAM is shared by every board, and the tightest margins against the pool-arena
    // link ASSERT are f302nucleo's 608 B and microbit's 768 B. The two workers write it in
    // sequence (each is joined on CH_DONE before the next spawns), so the load/store pair
    // below needs no atomicity.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(periph_reg_write_worker, nullptr, "prwW", 10, caps, 1,
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
            holder = kos::thread::create(periph_reg_write_held_worker,
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
    // compare. arch/sim/sim.cc models a write-PV-only register block over real host pages so
    // this arm reaches both, through the same dispatch arm and the same caller_holds_mmio_reg.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // Try every candidate, in the sim's own order: exactly one is mapped, and which
        // one depends on the host's address space, not on this test.
        kos::thread::Handle w;
        for (uintptr_t b : PVS_BASES)
        {
            w = kos::thread::create(pvs_worker, reinterpret_cast<void*>(b), "pvsW", 10,
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
            // Fails rather than skips: the sim maps the block at one of PVS_BASES, so a
            // refusal points at the grant path, the sim's mapping loop, or a drift between
            // the two lists. A skip would fail the sim gate anyway
            // (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            tap::fail("no candidate DEV window at the sim's fake register block "
                      "(last rc %d): PVS_BASES drifted from SIM_PVREG_BASES, or the "
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
    // syscall_thread.cc refuses a privileged child to an unprivileged caller. That refusal
    // is the whole of "only idle is privileged once boot is over".
    //
    // Called from ROOT, which is unprivileged, so the refusal costs no thread slot and
    // no arena block (the 16 KiB boards have neither to spare). A posture in which root
    // were privileged turns this red rather than vacuous: the spawn would succeed.
    void escalate_noop(void*) {}
    void t_privileged_spawn_refused()
    {
        TAP_CHECK(kos::thread::create(escalate_noop, nullptr, "escd", 10, KOS_POLICY_FIFO,
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
        // t0 is read BEFORE the spawn: the target outranks root, so it can reach its sleep
        // before the spawn returns, and a t0 taken afterwards would exclude that head start
        // from an interval the check below requires to CONTAIN the sleep.
        uint64_t t0 = kos_clock_now();
        // One slot at a time: the target is joined before the stranger is spawned.
        auto w = kos::thread::create(join_target, nullptr, "join", 10);
        TAP_CHECK(w.valid());
        // PARKED path, and the elapsed time is what says so: a join that answers 0 in less
        // than JOIN_PARK_NS answered from the EXITED early return instead.
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(join_stranger, nullptr, "jstr", 10, caps, 2);
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
        auto first = kos::thread::create(join_probe, nullptr, "jgn1", 10);
        TAP_CHECK(first.valid());
        TAP_CHECK(first.join() == 0); // EXITED, and its slot is now the lowest reclaimable
        kos_thread_t const stale = first.id();
        auto second = kos::thread::create(join_probe, nullptr, "jgn2", 10);
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
        auto w = kos::thread::create(join_slow, nullptr, "jslo", 10);
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
    // KOS_TASK_NONE and no live task is ever named by it. The gate is CREATORSHIP and takes
    // a second thread to witness, so this arm covers what one thread can see: every refusal
    // the codec produces, and that a hold, once dropped, names nothing.
    void t_task_handles()
    {
        // The out-pointer is validated BEFORE the group exists: a null one is malformed and a
        // misaligned one would take a privileged store the kernel must not make. Checked first,
        // because a mint that cannot deliver its handle leaves a task nothing can name.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, nullptr) == -KOS_EINVAL);
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        TAP_CHECK(task != KOS_TASK_NONE); // the bias is what makes this assertion possible
        // The two words nothing can mint: the sentinel, and a generation the slot never held.
        TAP_CHECK(kos_task_kill(KOS_TASK_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(task ^ 0xFFFF0000u) == -KOS_EBADF);
        // An out-of-range index, whatever generation rides it.
        TAP_CHECK(kos_task_kill(0x0000FFFFu) == -KOS_EBADF);
        // AN IMPLICIT TASK IS UNNAMEABLE: idle's task is created first (kmain makes idle
        // before root) and root's second, so slots 0 and 1 hold them and the biased codec
        // names those two handles 1 and 2 at generation 0. Neither slot is ever freed. IDLE's
        // carries the KERNEL domain, the whole arena at R|W, so a handle that resolved would
        // hand an unprivileged thread the arena. Root's is reachable to root's own spawns by
        // construction and to nothing else: a plain spawn joins the CALLER's task, never a
        // named one.
        TAP_CHECK(kos_task_kill(1u) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(2u) == -KOS_EBADF);
        struct kos_thread_params ip = {};
        ip.entry = join_probe;
        ip.name = "timp";
        ip.prio = 10;
        ip.task = 2u; // root's own implicit task
        kos_thread_t ih = KOS_THREAD_NONE;
        TAP_CHECK(kos_thread_create(&ip, &ih) == -KOS_EBADF);
        TAP_CHECK(ih == KOS_THREAD_NONE);

        // A group holding no thread still RESERVES its slot: an implicit task minted by the
        // spawn below must not be handed the slot `task` is sitting in, or that spawn would
        // overwrite the creator tag and `task` would stop naming anything.
        auto probe = kos::thread::create(join_probe, nullptr, "trsv", 10);
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
        if (kos_thread_create(&p, &h) == -KOS_EPERM)
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
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{ep, CH_FULL}};
        auto stranger = kos::thread::create(
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
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
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
        TAP_CHECK(kos_thread_create(&p, &h) == -KOS_EINVAL);
        TAP_CHECK(h == KOS_THREAD_NONE);
        // Same spawn against a handle no slot answers.
        p.mem_base = nullptr;
        p.mem_size = 0;
        p.task = KOS_TASK_NONE ^ 0x0000FFFFu; // a real generation over no index
        TAP_CHECK(kos_thread_create(&p, &h) == -KOS_EBADF);
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
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
        auto member = kos::thread::create(task_member, nullptr, "tmbr", 10, KOS_POLICY_FIFO, 0,
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
    // without preempting it and root cannot run again until the worker PARKS. Without that,
    // both arms below pass vacuously, the worker having died at the entry to a wait it never
    // reached.
    bool stage_a_parked_slay_worker(kos::thread::Handle* out, kos_cap_t park)
    {
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        *out = kos::thread::create_caps(slay_window_worker, nullptr, "slay", 10, caps, 2);
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
        auto probe = kos::thread::create(join_probe, nullptr, "slgn", 10);
        TAP_CHECK(probe.valid());
        TAP_CHECK(probe.join(JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_thread_slay(probe.id(), JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(slay_gate_probe, nullptr, "slst", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // parenthood, and there is no capability to delegate it with
    }

    // --- Slay: the group form ---------------------------------------------------
    // 0 here means a condition no other call in the ABI waits on: the group is EMPTY.
    void t_task_slay_group()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        g_slay_window = 0;
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        auto member = kos::thread::create(slay_window_worker, nullptr, "tsly", 10,
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
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(task_slay_stranger,
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
    // The middle guarantee level. It needs a victim that CANNOT be scheduled while the
    // deadline runs, so a higher-priority thread holds the CPU across it: the starvation
    // hazard the timeout exists to make visible instead of hiding in an unbounded park.
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
        auto hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
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
        // lose all of it between the two: under an interrupt-driven console the caller blocks
        // there, the hog spends its window, and the victim dies at once because nothing
        // outranks it. slay returning 0 then is correct, so asserting the timeout without
        // this check measures nothing. The hog has exited, so join it and stage a fresh one.
        for (int attempt = 0; attempt < 2 and not hog_window_open(); attempt++)
        {
            (void)hog.join(JOIN_GENEROUS_US);
            g_hog_start_ns = 0;
            hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
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
            tap::skip("hog window spent before the slay, starvation not established");
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
    // THE CEILING IS A DIFFERENT ONE WHERE A BACKEND TRANSLATES. No descriptor is seated
    // there, so the self-grant spends no region budget; what runs out is the RESERVATION
    // list, which allocation spends one slot of and never frees, and F10 re-keys the
    // -KOS_ENOMEM onto exactly that. The loop therefore asks the space how many slots are
    // left and takes one more than that, so it ends refused whatever the bound is.
    int sg_budget()
    {
#if KICKOS_HAVE_ASPACE
        return static_cast<int>(kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0)) + 1;
#else
        return SG_MAX;
#endif
    }
    Atomic<int, Order::RELAXED> g_sg_ok{0};       // descriptors accepted before the ceiling
    Atomic<int32_t, Order::RELAXED> g_sg_refusal{0}; // the code that ended the loop
    Atomic<int32_t, Order::RELAXED> g_sg_badsize{0};
    Atomic<int, Order::RELAXED> g_sg_readback{-1};

    void selfgrant_worker(void*)
    {
        // Size-0 refusal costs no arena and no descriptor. The address is a valid
        // stack local, so the refusal is about the SIZE alone.
        int probe = 0;
        g_sg_badsize = kos_mem_self_grant(&probe, 0, 0);
        int const budget = sg_budget();
        for (int i = 0; i < budget; i++)
        {
            void* p = kos_ram_alloc(1);
            if (p == nullptr)
            {
#if KICKOS_HAVE_ASPACE
                // Frames still free is what separates the reservation ceiling from a pool
                // that has actually run out.
                if (kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0) == 0
                    and kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) != 0)
                {
                    g_sg_refusal = -KOS_ENOMEM;
                    break;
                }
#endif
                g_sg_refusal = 0; // arena, not budget: the parent skips rather than fails
                break;
            }
            int32_t const rc = kos_mem_self_grant(p, 1, 0);
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
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(selfgrant_worker, nullptr, "sgW", 10, caps, 1,
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
    // Wherever arch_ram_region_size is granular rather than pow2 (a min-region-0 backend
    // like nRF51/LX6, and every base+limit MPU), three granules round to three granules, so
    // rsz - 1 is not an alignment mask and a block kos_ram_alloc handed out must still
    // self-grant. The sizes must be granule-derived: a constant only discriminates at one
    // granule. Consecutive 3-granule blocks step through the granule residues of 4 granules,
    // so within three blocks one base is not 4-granule aligned, exactly the base an ungated
    // pow2 mask check refuses.
    //
    // Registered BEFORE domain_share, not with mem_self_grant: on microbit the
    // pool cannot host a worker by the time the budget test runs, and this probe
    // is only discriminating on exactly such a no-MPU board.
    //
    // THE RESIDUE WALK IS A BUMP-ARENA ARGUMENT. Where the allocator is a first-fit frame
    // bitmap, consecutive blocks do not step through residues and the loop can pick an
    // already-4-granule-aligned base, which makes the arm NON-DISCRIMINATING rather than
    // wrong: the fallback is the first block, and a self-grant of a 3-granule block is still
    // what runs. It is left registered there because the size, not the base, is what
    // mem_self_grant is being asked about.
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
            g_sgnp_rc = kos_mem_self_grant(pick, want, 0);
            g_sgnp_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    // --- Which region-encoding mode is live on this board ----------------------
    // The bump allocator's step for a 3-granule request IS the mode: a base+limit
    // backend reserves 3 granules, a pow2 backend rounds to 4.
    //
    // A BUMP ARENA ONLY. Under translation there is no region encoding to name, and the
    // subtraction below is not a stride either: kos_ram_alloc reserves frames out of a
    // first-fit bitmap, so an earlier free makes two consecutive results non-monotonic and
    // `step` reports pool state rather than shaping. Allocation order is not public API on
    // that backend, so the arm is not registered there; the map editor answers this instead
    // (aspace_model, aspace_seam).
#if not KICKOS_HAVE_ASPACE
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
#endif

    void t_selfgrant_nonpow2()
    {
        // Unprivileged + AUTH_MEMORY, like t_selfgrant: a privileged caller is
        // answered "already reachable" before the geometry is ever examined.
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(sgnp_worker, nullptr, "sgNP", 10, caps, 1,
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
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("call_reg_fastpath", t_call_reg_fastpath);       // register form taken; buffer form above the budget
#endif
    TAP_ADD("call_from_root", t_call_from_root);             // root is a pool slot, so it may call (fast+slow)
    TAP_ADD("call_truncation", t_call_truncation);           // request + reply datagram clamp
    TAP_ADD("call_double_reply", t_call_double_reply);       // one-shot cap -> second reply -KOS_EBADF
    TAP_ADD("call_server_death", t_call_server_death);       // die mid-xact -> caller EPIPE (teardown arm)
    TAP_ADD("call_prepop_death", t_call_prepop_death);       // die pre-pop -> caller EPIPE (recv_holders 0)
    TAP_ADD("call_donation", t_call_donation);               // D1 donation keeps the spoiler off the xact
#undef TAP_ADD
// Region 2.
#if KICKOS_SELFTEST_PART == 0 || KICKOS_SELFTEST_PART == 2
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("call_donation_hold", t_call_donation_hold);     // D3: a reply donor survives an unrelated recompute
    TAP_ADD("call_donation_slow", t_call_donation_slow);     // D3: same, via the recv-side mint
    TAP_ADD("call_donation_pending", t_call_donation_pending); // D3: a SEND_WAIT donor does too
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
    TAP_ADD("readable_global", t_readable_global);      // read buffer in app rodata
    TAP_ADD("authority_cap", t_authority_cap);          // authority word: both arms of the gates
    TAP_ADD("periph_enable_unheld", t_periph_enable_unheld); // possession is the whole periph-enable gate
    TAP_ADD("periph_reg_write_unheld", t_periph_reg_write_unheld); // same gate + the offset bound + the chip refusal
#if KICKOS_ARCH_SIM
    TAP_ADD("periph_reg_write_mask", t_periph_reg_write_mask); // allowlist match + the per-entry value mask
#endif
#undef TAP_ADD
// Region 3.
#if KICKOS_SELFTEST_PART == 0 || KICKOS_SELFTEST_PART == 3
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
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
#if KICKOS_HAVE_ASPACE
    TAP_ADD("caller_stack_arena", t_caller_stack_arena); // app .bss caller stack is out of arena
#endif
    // Here, not beside mem_self_grant: see the run-order note at t_selfgrant_nonpow2.
    TAP_ADD("mem_self_grant_nonpow2", t_selfgrant_nonpow2); // non-pow2 region self-grants
#if not KICKOS_HAVE_ASPACE
    TAP_ADD("region_mode", t_region_mode);                  // which region-encoding mode is live
#endif
    TAP_ADD("domain_share", t_domain_share); // two tasks, one reserved range handed to each
    TAP_ADD("mmio_grant", t_mmio_grant);     // MMIO-grant boundary: privileged-only + encodable-only
#if KICKOS_HAVE_MPU
    TAP_ADD("stackbase_arena", t_stackbase_arena); // unprivileged out-of-arena stack_base refused
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("grant_reserved", t_grant_reserved);   // Rule 7: overlap matrix + RAM/DEV admission (probe syscall)
    TAP_ADD("dev_window_exclusive", t_dev_window_exclusive); // one holder per DEV window (-KOS_EBUSY)
#endif
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("aspace_seam", t_aspace_seam);           // granule, the three memory types, the frame pool
    TAP_ADD("aspace_model", t_aspace_model);         // S2/F7: what the machine reports, against the manuals
    TAP_ADD("aspace_map_cycle", t_aspace_map_cycle); // map, write, read back, unmap, gone
    TAP_ADD("aspace_translate", t_aspace_translate); // two virtual pages, one frame: not an identity map
    TAP_ADD("aspace_refusals", t_aspace_refusals);   // the map editor's refusal set, as one word
    TAP_ADD("aspace_span", t_aspace_span);           // a range crossing two table boundaries
    TAP_ADD("aspace_balance", t_aspace_balance);     // destroy returns every frame a space took
    TAP_ADD("aspace_domain_balance", t_aspace_domain_balance); // a dropped domain returns its space
    TAP_ADD("aspace_forced_unwind", t_aspace_forced_unwind); // T8b: a refused allocation, swept through the create
    TAP_ADD("aspace_churn", t_aspace_churn);         // T8b: processes and refusals churned, frames and roots back
    TAP_ADD("stack_is_frames", t_stack_is_frames);   // a stack is frames with a guard, and they come back
    TAP_ADD("aspace_two_spaces_same_grant", t_aspace_two_spaces_same_grant); // one block, two spaces
    TAP_ADD("aspace_two_spaces_no_grant", t_aspace_two_spaces_no_grant);     // no grant, still two spaces
    TAP_ADD("process_private_data", t_process_private_data); // one address, three frames: F2's process witness
    TAP_ADD("task_siblings_share", t_task_siblings_share);   // two members of one group, one image
    TAP_ADD("task_handoff_readback", t_task_handoff_readback); // F10's handoff, both consumers
    TAP_ADD("task_handoff_donor_exits", t_task_handoff_donor_exits); // the donor dies first
    TAP_ADD("self_grant_cross_task", t_self_grant_cross_task); // F10: another task's reservation, refused
    TAP_ADD("reservation_teardown", t_reservation_teardown);   // F10: a never-mapped reservation, released at death
    TAP_ADD("parked_frame_hostile", t_parked_frame_hostile); // a sibling scribbles a parked member's frame
    TAP_ADD("split_access", t_split_access);       // a virtual range over two non-adjacent frames
    TAP_ADD("process_ipc_same_addr", t_process_ipc_same_addr); // one address, two processes, message + info
    TAP_ADD("process_call_reply", t_process_call_reply);       // call and reply across two processes
    TAP_ADD("grant_kernel_word_refused", t_grant_kernel_word_refused); // F6: a kernel address self-grant, refused by name
    TAP_ADD("self_grant_retype", t_self_grant_retype); // the typed short circuit, both directions
    TAP_ADD("reent_seating", t_reent_seating);         // no app-half write for a space-less thread
    TAP_ADD("aspace_acquire_balance", t_aspace_acquire_balance); // one release per acquire taken
    TAP_ADD("map_tlbi_elided", t_map_tlbi_elided); // an unpublished space caches nothing to drop
    // LAST of the block: it drops the space holding the image's own data pages for good, and
    // every process created after it copies the snapshot instead.
    TAP_ADD("process_data_template", t_process_data_template); // the snapshot, once root is gone
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST) && KICKOS_FAULT_ISOLATION
    TAP_ADD("fault_kills_task", t_fault_kills_task); // F5: a translation fault ends the TASK, root survives
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
