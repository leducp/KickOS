// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Semaphore ping-pong and IPC microbenchmarks. The kernel prints its benchmark tables on the
// kernel console, so benchmark variants select kickos_services_none.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/irq_free.h>
#include <kickos/sys/init.h>
#include <kickos/libc/fmt.h>
#include <kickos/libc/string.h>

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // A line below the board's free block injects as a silent no-op and reports a clean zero.
    constexpr int BENCH_IRQ_LINE = KICKOS_IRQ_FREE_BASE + 0;
    constexpr int BENCH_E2E_LINE = KICKOS_IRQ_FREE_BASE + 1;
    constexpr int IRQ_SAMPLES = KOS_BENCH_SAMPLES_MAX;
    constexpr uint32_t E2E_PAIRS_PER_CORE = 2;
    // Per PASS, the sweep running two passes per kernel core: the fewest that make the local row
    // carry the 1000 samples a p99 is stated at.
    constexpr uint32_t E2E_ROW_SAMPLES = 1000;
    constexpr int E2E_SAMPLES = static_cast<int>(
        (E2E_ROW_SAMPLES + KICKOS_KERNEL_CORES * E2E_PAIRS_PER_CORE - 1u)
        / (KICKOS_KERNEL_CORES * E2E_PAIRS_PER_CORE));
    // The sweep size e2e_sweep runs: the denominator the kernel prints beside the raises it let
    // through.
    constexpr uint32_t E2E_SWEEP_PASSES =
        KICKOS_KERNEL_CORES * E2E_PAIRS_PER_CORE * static_cast<uint32_t>(E2E_SAMPLES);
    // A yield count, not a time: the bound on each wait for the waiter to park or to close.
    constexpr int E2E_TRIES = 2000;
    // The tare, once per run: the return to userspace, the device read and the trap back in.
    constexpr int E2E_TARE_SAMPLES = 64;
    // An MPU rounds a grant up to what it can describe and the rounded window must still lie
    // inside the block that was reserved.
    constexpr uint32_t E2E_DEV_BYTES = 4096;
    // Rounds per throughput window: a few seconds on a slow M0.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;
    // Per core per window.
    constexpr uint32_t DOORBELL_ROUNDS = KOS_BENCH_ROUNDS_MAX;
    // Bounds the walk only; the kernel's refusal ends it. A core set is a 32-bit mask.
    constexpr uint32_t CORE_WALK_MAX = 32;
    // Bounded so main RETURNS: root's exit reaches kickos_terminate, and a board built with
    // KICKOS_SHUTDOWN_TO_BOOTLOADER re-enters its bootloader without a button press.
    constexpr unsigned THROUGHPUT_REPORTS = 3;
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t TAG_CALLREPLY = 0;
    constexpr uint32_t TAG_W1 = 1;
    constexpr uint32_t TAG_W2 = 2;
    constexpr uint32_t TAG_W3 = 3;
    constexpr uint32_t TAG_PUSH = 4;
    constexpr uint32_t TAG_RESEAT = 5;

    // Printed only by an image carrying the scheduler's own counters (KICKOS_BENCH_SCHED).
    void sched_report(uint32_t tag)
    {
#if KICKOS_BENCH_SCHED
        (void)kos_bench(KOS_BENCH_OP_SCHED_PRINT, tag, 0);
#else
        (void)tag;
#endif
    }
#endif

    kos::Semaphore* g_a = nullptr;    // MAIN's caps
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    kos::Semaphore* g_resume = nullptr;
    Atomic<uint32_t, Order::RELAXED> g_rounds{0};

    // Child cap indices (a fresh child table makes handle == index). MAIN delegates the
    // sems per spawn in this order.
    constexpr int CH_A = 1;
    constexpr int CH_B = 2;
    constexpr int CH_GATE = 3;   // reporter-wake gate, delegated to player_b only
    constexpr int CH_RESUME = 4; // posted by the reporter as the next window opens, player_b only
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;
    // Both players on one core: the throughput row is the same-core handoff. Under a wider
    // mask the placement invariant would spread the equal-priority pair across cores.
    constexpr uint32_t PLAYER_CORES = 1u << 0;

    // A refused op returns -KOS_E*; the cycle ops answer 0 for "did not fire", so a
    // refusal must collapse to 0 for a caller's `!= 0` fired-check to hold.
    uint32_t bench_u32(uint32_t op, uint32_t a0, uint32_t a1)
    {
        int64_t const rc = kos_bench(op, a0, a1);
        if (rc < 0)
        {
            return 0;
        }
        return static_cast<uint32_t>(rc);
    }

    void player_a(void*) // caps: A@1, B@2
    {
        while (true)
        {
            kos_sem_wait(CH_A);
            kos_sem_post(CH_B);
        }
    }
    // Parks for the whole report, with player_a parked on A behind it: above one kernel core the
    // reporter's probes and sweeps take it off the players' core, and the pair runs there again.
    void player_b(void*) // caps: A@1, B@2, gate@3, resume@4
    {
        while (true)
        {
            kos_sem_wait(CH_B);
            uint32_t const round = g_rounds + 1;
            g_rounds = round;
            if ((round % ROUNDS_PER_REPORT) == 0)
            {
                kos_sem_post(CH_GATE);
                kos_sem_wait(CH_RESUME);
            }
            kos_sem_post(CH_A);
        }
    }

    // --- the end-to-end span ---------------------------------------------------
    // Both ends are spawned threads of root's own task, so root can place either.
    // The waiter holds the line, so nothing else in the image may.
    constexpr int CH_E2E_IRQ = 1;   // waiter: the claimed line, WAIT only
    constexpr int CH_E2E_READY = 2; // waiter: posted once, after the tare, before the first park
    constexpr int CH_E2E_NOTE = 3;  // waiter: the object that line signals, WAIT only
    constexpr int CH_E2E_GO = 1;    // raiser: one post per pass
    constexpr int CH_E2E_PASS = 2;  // raiser: posted when a pass has run its samples

    // Root attaches the line with an unbadged copy, so it raises bit 0.
    constexpr uint32_t E2E_LINE_BIT = 1u << 0;
    // The core the line is claimed on and its waiter pinned to above one kernel core; 0 asks
    // for the default set, which is the only one at one kernel core.
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t E2E_LINE_CORES = 1u << 0;
#else
    constexpr uint32_t E2E_LINE_CORES = 0;
#endif

    void* g_e2e_dev = nullptr;
    kos_thread_t g_e2e_tid = KOS_THREAD_NONE;
    kos_thread_t g_e2e_rid = KOS_THREAD_NONE;
    kos_cap_t g_e2e_ready = KOS_CAP_NONE;
    kos_cap_t g_e2e_go = KOS_CAP_NONE;
    kos_cap_t g_e2e_pass = KOS_CAP_NONE;
    Atomic<uint32_t, Order::RELAXED> g_e2e_stop{0};
    // Bumped by the waiter AFTER its close, so the handshake is outside every span.
    Atomic<uint32_t, Order::RELAXED> g_e2e_done{0};
    // 0 until the waiter reaches its loop; negative says why it did not.
    Atomic<int32_t, Order::RELAXED> g_e2e_grant_rc{0};
    // Passes whose set-up settled before their first raise, published by the pass handshake.
    Atomic<uint32_t, Order::RELAXED> g_e2e_settled{0};

    void e2e_waiter(void*)
    {
        if (kos_notify_bind(CH_E2E_NOTE) != 0)
        {
            kos_sem_post(CH_E2E_READY);
            return;
        }
        // Under an MPU, reachability is per thread, so root's own grant of this block does not
        // carry here; under translation it already does and this answers 0 again.
        g_e2e_grant_rc = kos_mem_self_grant(g_e2e_dev, E2E_DEV_BYTES, 0);
        if (g_e2e_grant_rc != 0)
        {
            kos_sem_post(CH_E2E_READY);
            return;
        }
        volatile uint32_t* const dev = static_cast<volatile uint32_t*>(g_e2e_dev);
        for (int i = 0; i < E2E_TARE_SAMPLES; i++)
        {
            (void)kos_bench(KOS_BENCH_OP_E2E_ARM, CH_E2E_IRQ, 0);
            (void)kos_bench(KOS_BENCH_OP_E2E_TARE, 0, 0);
            dev[1] = dev[0];
            (void)kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
        }
        kos_sem_post(CH_E2E_READY);
        while (g_e2e_stop == 0)
        {
            (void)kos_bench(KOS_BENCH_OP_E2E_ARM, CH_E2E_IRQ, 0);
            if (kos_notify_wait(CH_E2E_NOTE, E2E_LINE_BIT, KOS_TIMEOUT_NONE, nullptr) != 0)
            {
                break; // cancelled, or the object went away: nothing left to wake for
            }
            // The closing stamp is the next instruction after this read, so the span carries
            // the read and one trap and nothing else of this thread's.
            dev[1] = dev[0];
            (void)kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
            g_e2e_done = g_e2e_done + 1;
        }
    }

    // Ranks below the waiter, so every raise runs to the waiter's close before this thread is
    // scheduled again at one kernel core; above one the done counter is what orders it. Ends
    // only by cancellation, which is what unblocks its wait.
    void e2e_raiser(void*)
    {
        while (true)
        {
            if (kos_sem_wait(CH_E2E_GO) != 0)
            {
                break;
            }
            // The pass's own set-up is not a sample: the post that started it, and the placement
            // that moved root off this core, settle before the first raise.
            int q = 0;
            while (q < E2E_TRIES and kos_bench(KOS_BENCH_OP_E2E_QUIET, 0, 0) == 0)
            {
                kos_yield();
                q++;
            }
            if (q < E2E_TRIES)
            {
                g_e2e_settled = g_e2e_settled + 1;
            }
            for (int i = 0; i < E2E_SAMPLES; i++)
            {
                uint32_t const before = g_e2e_done;
                int t = 0;
                while (t < E2E_TRIES and kos_bench(KOS_BENCH_OP_E2E_RAISE, 0, 0) != 0)
                {
                    kos_yield();
                    t++;
                }
                if (t >= E2E_TRIES)
                {
                    continue; // the waiter never parked; the probe line reports the shortfall
                }
                for (t = 0; t < E2E_TRIES and g_e2e_done == before; t++)
                {
                    kos_yield();
                }
            }
            kos_sem_post(CH_E2E_PASS);
        }
    }

    // The waiter outranks both root and the raiser.
    bool e2e_start()
    {
        g_e2e_dev = kos_ram_alloc(E2E_DEV_BYTES);
        if (g_e2e_dev == nullptr or kos_mem_self_grant(g_e2e_dev, E2E_DEV_BYTES, 0) != 0)
        {
            kickos::emit("  e2e: SKIP (no device window: allocation or grant refused)\n");
            return false;
        }
        for (uint32_t i = 0; i < 4; i++)
        {
            static_cast<volatile uint32_t*>(g_e2e_dev)[i] = 0;
        }
        kos_cap_t irq = KOS_CAP_NONE;
#if KICKOS_KERNEL_CORES > 1
        // A line is claimed by a thread pinned where it runs; only the claim needs root there.
        kos_thread_t const self = kos_thread_self();
        (void)kos_thread_set_affinity(self, E2E_LINE_CORES);
#endif
        int const crc = kos_irq_claim(BENCH_E2E_LINE, KOS_IRQ_EDGE, &irq);
#if KICKOS_KERNEL_CORES > 1
        (void)kos_thread_set_affinity(self, 0);
#endif
        if (crc != 0)
        {
            char cs[96];
            ksnprintf(cs, sizeof(cs), "  e2e: SKIP (line %u claim refused, rc=%d)\n",
                      static_cast<unsigned>(BENCH_E2E_LINE), static_cast<int>(crc));
            kickos::emit(cs);
            return false;
        }
        kos_cap_t note = KOS_CAP_NONE;
        if (kos_notify_create(&note) != 0 or kos_irq_bind_notify(irq, note) != 0)
        {
            kos_handle_close(note);
            kos_handle_close(irq);
            kickos::emit("  e2e: SKIP (the line could not be attached to a notification)\n");
            return false;
        }
        // The claim leaves the line MASKED and the waiter's first arm happens after the ready
        // handshake, so a raise before that would land on a masked line and be discarded.
        // Arming needs the line attached, which is why this follows the attach.
        kos_irq_ack(irq);
        if (kos_sem_create(0, &g_e2e_ready) != 0)
        {
            kos_handle_close(note);
            kos_handle_close(irq);
            return false;
        }
        kos_cap_grant caps[] = {{irq, KOS_CAP_WAIT},
                                {g_e2e_ready, CH_FULL},
                                {note, KOS_CAP_WAIT}};
        auto w = kos::thread::create_caps(e2e_waiter, nullptr, "e2ewait", 15, caps, 3,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                          KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE, nullptr, 0,
                                          E2E_LINE_CORES);
        kos_handle_close(note); // the waiter's bind and the line's attach both hold their own
        kos_handle_close(irq); // the waiter is the sole holder: its exit frees the line
        if (not w.valid())
        {
            kos_sem_destroy(g_e2e_ready);
            kickos::emit("  e2e: SKIP (thread pool too small for the waiter)\n");
            return false;
        }
        g_e2e_tid = w.id();
        kos_sem_wait(g_e2e_ready);
        if (g_e2e_grant_rc != 0)
        {
            // It already returned. Left named, the sweep below would raise into a line whose
            // owner is gone and report a shortfall instead of this refusal.
            (void)kos_thread_join(g_e2e_tid, KOS_TIMEOUT_NONE);
            g_e2e_tid = KOS_THREAD_NONE;
            kos_sem_destroy(g_e2e_ready);
            kickos::emit("  e2e: SKIP (the waiter cannot reach the device window)\n");
            return false;
        }
        if (kos_sem_create(0, &g_e2e_go) != 0 or kos_sem_create(0, &g_e2e_pass) != 0)
        {
            kickos::emit("  e2e: SKIP (no semaphore for the raiser handshake)\n");
            return false;
        }
        kos_cap_grant rcaps[] = {{g_e2e_go, CH_FULL}, {g_e2e_pass, CH_FULL}};
        auto r = kos::thread::create_caps(e2e_raiser, nullptr, "e2erais", 3, rcaps, 2);
        if (not r.valid())
        {
            kickos::emit("  e2e: SKIP (thread pool too small for the raiser)\n");
            return false;
        }
        g_e2e_rid = r.id();
        return true;
    }

    // The line is delivered on its claim core whoever injects it, and the waiter is pinned
    // there, so every wake is local and the raiser's placement moves only where the raise is
    // injected.
    void e2e_sweep()
    {
        if (g_e2e_tid == KOS_THREAD_NONE or g_e2e_rid == KOS_THREAD_NONE)
        {
            return;
        }
        g_e2e_settled = 0;
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            for (uint32_t d = 0; d < E2E_PAIRS_PER_CORE; d++)
            {
                (void)kos_thread_set_affinity(g_e2e_rid, 1u << c); // -KOS_ENOSYS at one core
                kos_sem_post(g_e2e_go);
                kos_sem_wait(g_e2e_pass);
            }
        }
        char s[64];
        ksnprintf(s, sizeof(s), "  e2e-settle: settled=%u/%u\n",
                  static_cast<unsigned>(g_e2e_settled.load()),
                  static_cast<unsigned>(KICKOS_KERNEL_CORES * E2E_PAIRS_PER_CORE));
        kickos::emit(s);
    }

    void e2e_stop()
    {
        g_e2e_stop = 1;
        // Both are parked on something only cancellation ends.
        if (g_e2e_rid != KOS_THREAD_NONE)
        {
            (void)kos_thread_kill(g_e2e_rid);
            (void)kos_thread_join(g_e2e_rid, KOS_TIMEOUT_NONE);
            g_e2e_rid = KOS_THREAD_NONE;
        }
        if (g_e2e_tid != KOS_THREAD_NONE)
        {
            (void)kos_thread_kill(g_e2e_tid);
            (void)kos_thread_join(g_e2e_tid, KOS_TIMEOUT_NONE);
            g_e2e_tid = KOS_THREAD_NONE;
        }
        if (g_e2e_go != KOS_CAP_NONE)
        {
            kos_sem_destroy(g_e2e_go);
            kos_sem_destroy(g_e2e_pass);
            g_e2e_go = KOS_CAP_NONE;
        }
        if (g_e2e_ready != KOS_CAP_NONE)
        {
            kos_sem_destroy(g_e2e_ready);
            g_e2e_ready = KOS_CAP_NONE;
        }
    }

    // The reporter is the ROOT thread; the two players and the end-to-end pair are the only
    // slots the bench adds to root's own.
    void reporter_loop()
    {
        int64_t const setup_rc = kos_bench(KOS_BENCH_OP_IRQ_SETUP, BENCH_IRQ_LINE, 0);
        if (setup_rc != 0)
        {
            char ss[96];
            ksnprintf(ss, sizeof(ss), "  irq: SKIP (line %u attach refused, rc=%d)\n",
                      static_cast<unsigned>(BENCH_IRQ_LINE), static_cast<int>(setup_rc));
            kickos::emit(ss);
        }
        (void)e2e_start();

        for (unsigned rep = 0; rep < THROUGHPUT_REPORTS; rep++)
        {
            (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
            // The window is the players' burst alone: they are parked from the gate until
            // this release.
            uint32_t const prev_rounds = g_rounds;
            uint64_t const prev_ns = kos::clock_now();
            if (rep == 0)
            {
                g_a->post();
            }
            else
            {
                g_resume->post();
            }
            g_gate->wait();

            uint64_t now_ns = kos::clock_now();
            uint32_t rounds = g_rounds;
            uint64_t switches = static_cast<uint64_t>(rounds - prev_rounds) * 2ull; // 2 switches/round
            uint64_t d_ns = now_ns - prev_ns;

            uint32_t sw_per_s = 0;
            uint32_t ns_per_sw = 0;
            if (d_ns != 0 and switches != 0)
            {
                sw_per_s = static_cast<uint32_t>(switches * 1000000000ull / d_ns);
                ns_per_sw = static_cast<uint32_t>(d_ns / switches);
            }

            // Every %u argument is cast to `unsigned`: uint32_t is `unsigned long` on the newlib
            // targets and `unsigned` elsewhere, and `unsigned` is 32-bit everywhere KickOS runs.
            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      static_cast<unsigned>(sw_per_s), static_cast<unsigned>(ns_per_sw),
                      static_cast<unsigned>(switches),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kickos::emit(s);
#if KICKOS_KERNEL_CORES > 1
            sched_report(TAG_W1);
#endif

            // The kernel places this thread before each burst and refuses a core it does not
            // schedule, which is what ends the walk; at one kernel core the first call is
            // refused and no round runs.
            for (uint32_t c = 0; c < CORE_WALK_MAX; c++)
            {
                if (kos_bench(KOS_BENCH_OP_DOORBELL_PROBE, c, DOORBELL_ROUNDS) < 0)
                {
                    break;
                }
            }

            // Returns the SWITCH sample count, 0 where switch.S brackets no cycles.
            uint32_t const scnt = bench_u32(KOS_BENCH_OP_DIST_PRINT, 0, 0);
            if (scnt == 0)
            {
                continue;
            }

            (void)kos_bench(KOS_BENCH_OP_IRQ_SWEEP, IRQ_SAMPLES, 0);
            (void)kos_bench(KOS_BENCH_OP_IRQ_WCASE, IRQ_SAMPLES, 0);
            e2e_sweep();
            (void)kos_bench(KOS_BENCH_OP_E2E_PRINT, E2E_SWEEP_PASSES, 0);
        }
        e2e_stop();
    }

    // Both call/reply peers are SPAWNED, so the figure is worker-to-worker and not root's.
    constexpr uint32_t CALLREPLY_REPS = 20000;

    // A peer MUST outrank root (prio KICKOS_PRIO_MIN + 1 == 2): it posts `done` as its last
    // act but reaches EXITED only afterwards, and root preempting it on that post leaves the
    // peer READY, holding a slot ThreadPool::alloc cannot reclaim for the next sweep step: on
    // a 3-slot pool that spawn is -KOS_ENOMEM.
    constexpr uint8_t CR_PRIO = 4;

    // Written by measure_callreply BEFORE it spawns either peer, so neither peer races it.
    Atomic<uint32_t, Order::RELAXED> g_cr_len{16};

    // Same write discipline as g_cr_len.
    Atomic<uint32_t, Order::RELAXED> g_cr_donating{0};

    // Non-zero puts the caller on kos_call_generic, which issues KOS_SYS_CALL without
    // attempting the register form. Same write discipline as g_cr_len.
    Atomic<uint32_t, Order::RELAXED> g_cr_generic{0};

    void callreply_server(void*) // caps: E(WAIT)@1, done@2
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 1;
        opts.timeout_us = KOS_TIMEOUT_NONE;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            long const n = kos_reply_recv(reply_cap, buf,
                                          kos_call_lens_pack(reply_len, sizeof(buf)), &opts);
            reply_cap = KOS_CAP_NONE;
            if (n < 0 or opts.info.reply_cap == KOS_CAP_NONE)
            {
                // A reply-less message is measure_callreply's stop sentinel: a parked
                // receiver pins its own WAIT cap, so nothing but a message ends that park.
                break;
            }
            reply_cap = opts.info.reply_cap;
            reply_len = static_cast<size_t>(n);
        }
        if (reply_cap != KOS_CAP_NONE)
        {
            kos_reply(reply_cap, buf, reply_len);
        }
        kos_sem_post(2);
    }
    void callreply_caller(void*) // caps: E(SIGNAL)@1, done@2
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        size_t const len = static_cast<size_t>(g_cr_len);
        for (unsigned i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i);
        }
        // Selected OUTSIDE the timed loop, so neither arm carries the other's branch.
        bool const generic = (g_cr_generic != 0);
        uint64_t t0 = kos::clock_now();
        uint32_t reps = 0;
        if (generic)
        {
            while (reps < CALLREPLY_REPS)
            {
                if (kos_call_generic(1, buf, len, len) < 0)
                {
                    break;
                }
                reps++;
            }
        }
        else
        {
            while (reps < CALLREPLY_REPS)
            {
                // In place: the reply lands back in the request buffer.
                if (kos_call(1, buf, len, len) < 0)
                {
                    break;
                }
                reps++;
            }
        }
        uint64_t d_ns = kos::clock_now() - t0;

        if (reps == CALLREPLY_REPS)
        {
            uint32_t rt_per_s = 0;
            uint32_t ns_per_rt = 0;
            if (d_ns != 0)
            {
                rt_per_s = static_cast<uint32_t>(
                    static_cast<uint64_t>(CALLREPLY_REPS) * 1000000000ull / d_ns);
                ns_per_rt = static_cast<uint32_t>(d_ns / CALLREPLY_REPS);
            }
            char const* shape = "";
            if (g_cr_donating != 0)
            {
                shape = " [caller outranks server, D1 donates]";
            }
            char const* path = "";
            if (generic)
            {
                path = " [generic path]";
            }
            char s[256];
            ksnprintf(
                s, sizeof(s),
                "  call/reply: %u B  %u ns/round-trip  (%u round-trips/s over %u calls / %u ms)%s%s\n",
                static_cast<unsigned>(len), static_cast<unsigned>(ns_per_rt),
                static_cast<unsigned>(rt_per_s), static_cast<unsigned>(CALLREPLY_REPS),
                static_cast<unsigned>(d_ns / 1000000ull), path, shape);
            kickos::emit(s);
        }
        kos_sem_post(2);
    }
    // endpoint_call's D1 donation is guarded on caller_prio strictly greater than
    // server_prio, so an equal-priority step leaves that phase row with zero samples.
    // Both must stay above root (see CR_PRIO).
    void measure_callreply(uint32_t len, uint8_t caller_prio, uint8_t server_prio,
                           uint32_t generic)
    {
        g_cr_len = len;
        uint32_t donating = 0;
        if (caller_prio > server_prio)
        {
            donating = 1;
        }
        g_cr_donating = donating;
        g_cr_generic = generic;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kickos::emit("  call/reply: SKIP (no endpoint)\n");
            return;
        }
        kos::Semaphore done(0);
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}, {done.id(), CH_FULL}};   // E(WAIT)@1, done@2
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}, {done.id(), CH_FULL}}; // E(SIGNAL)@1, done@2
        auto sv = kos::thread::create_caps(callreply_server, nullptr, "cr_srv", server_prio, scaps, 2);
        auto cl = kos::thread::create_caps(callreply_caller, nullptr, "cr_cl", caller_prio, ccaps, 2);
        if (not sv.valid() or not cl.valid())
        {
            // A lone peer holds its slot for the whole run unless drained HERE: the server
            // needs the sentinel, and the caller wakes -KOS_EPIPE only once recv_holders
            // reaches 0, which is this close.
            unsigned char stop = 0;
            if (sv.valid())
            {
                (void)kos_send(ep, &stop, 1);
                done.wait();
            }
            kos_handle_close(ep);
            if (cl.valid())
            {
                done.wait();
            }
            kickos::emit("  call/reply: SKIP (thread pool too small)\n");
            return;
        }
        done.wait(); // caller finished + printed
        done.wait(); // server ran all REPS and exited
        kos_handle_close(ep);
    }

    // The slope across these sizes is TWICE the per-byte cost of the endpoint copy: a round
    // trip copies the request in and the reply back, both under one IrqLock.
    constexpr uint32_t CR_SPANS[] = {8, 16, 32, 64, 128, 256};

    // One size only: the D1 cost does not scale with the message.
    constexpr uint32_t CR_DONATE_SPAN = 32;

    // A distribution bucket counts in uint32_t (kernel/include/kickos/bench_hist.h), so 2^32
    // samples to one slot wrap silently. Each call/reply rep and each ping-pong round drives two
    // switches, each posting one sample to every switch-path slot, so this is the busiest slot to
    // an order of magnitude; the margin below carries the rest.
    constexpr uint64_t BENCH_BUSIEST_SLOT =
        2ull * (static_cast<uint64_t>(CALLREPLY_REPS)
                    * (2ull * (sizeof(CR_SPANS) / sizeof(CR_SPANS[0])) + 1ull)
                + static_cast<uint64_t>(THROUGHPUT_REPORTS) * ROUNDS_PER_REPORT);
    static_assert(BENCH_BUSIEST_SLOT < (1ull << 31),
                  "these rep counts drive a distribution slot towards a 32-bit wrap; the "
                  "percentiles under it would be wrong and no row would say so");
}

#if KICKOS_KERNEL_CORES > 1
namespace
{
    // --- the scheduler's workloads (docs/design-m9.4-rings.md, section G) ----------------
    // Every thread a workload spawns is joined before the next starts, and all of them outrank
    // root.
    constexpr uint32_t W_ROUNDS = 5000;
    constexpr uint8_t W_PRIO = 3;
    constexpr uint8_t W_HOG_PRIO = 6;
    constexpr uint8_t W_MOVED_PRIO = 4;
    constexpr uint32_t W_RR_QUANTUM_NS = 1000000;
    constexpr uint32_t W_PUSH_ROUNDS = 200;
    // How long the push probe waits on one handoff before it names the round and stops.
    constexpr uint64_t W_PUSH_GIVEUP_NS = 2000000000ull;
    // Spins between clock reads: the clock is a syscall, and a round that completes inside
    // this many never reads it, so a healthy probe adds nothing to the report it prints.
    constexpr uint32_t W_SPIN_POLL = 1u << 16;
    constexpr uint32_t W_RESEAT_ROUNDS = 500;

    // A ping-pong pair's caps: A@1, B@2. Root's own table is sized for the declared peak and
    // not for every pair at once, so it closes its handles once the players hold theirs and
    // joins the players instead of waiting on a third semaphore.
    constexpr int W_A = 1;
    constexpr int W_B = 2;

    void w_player_a(void*)
    {
        for (uint32_t i = 0; i < W_ROUNDS; i++)
        {
            kos_sem_wait(W_A);
            kos_sem_post(W_B);
        }
    }

    void w_player_b(void*)
    {
        for (uint32_t i = 0; i < W_ROUNDS; i++)
        {
            kos_sem_post(W_A);
            kos_sem_wait(W_B);
        }
    }

    struct Pair
    {
        kos::thread::Handle a;
        kos::thread::Handle b;
    };

    // Two players on `mask`; 0 is the default set. Both are joined by the caller.
    bool pair_spawn(uint32_t mask, Pair* out)
    {
        kos_cap_t a = KOS_CAP_NONE;
        kos_cap_t b = KOS_CAP_NONE;
        if (kos_sem_create(0, &a) != 0)
        {
            return false;
        }
        if (kos_sem_create(0, &b) != 0)
        {
            kos_handle_close(a);
            return false;
        }
        kos_cap_grant caps[] = {{a, CH_FULL}, {b, CH_FULL}};
        out->a = kos::thread::create_caps(w_player_a, nullptr, "w_a", W_PRIO, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, mask);
        out->b = kos::thread::create_caps(w_player_b, nullptr, "w_b", W_PRIO, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, mask);
        kos_handle_close(a);
        kos_handle_close(b);
        return out->a.valid() and out->b.valid();
    }

    // A player whose partner was refused waits for good, so a half pair is cancelled first.
    void pair_join(Pair const& p)
    {
        if (p.a.valid() != p.b.valid())
        {
            (void)p.a.kill();
            (void)p.b.kill();
        }
        if (p.a.valid())
        {
            (void)p.a.join(KOS_TIMEOUT_NONE);
        }
        if (p.b.valid())
        {
            (void)p.b.join(KOS_TIMEOUT_NONE);
        }
    }

    void w_line(char const* name, uint32_t switches, uint64_t d_ns)
    {
        char s[128];
        ksnprintf(s, sizeof(s), "  %s: %u switches in %u us\n", name,
                  static_cast<unsigned>(switches), static_cast<unsigned>(d_ns / 1000u));
        kickos::emit(s);
    }

    // W2: one pinned pair per core, all at once.
    void w2_pairs()
    {
        uint32_t const n = KICKOS_KERNEL_CORES;
        Pair pairs[4] = {};
        uint32_t spawned = 0;
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        uint64_t const t0 = kos::clock_now();
        for (uint32_t c = 0; c < n and c < 4; c++)
        {
            if (not pair_spawn(1u << c, &pairs[c]))
            {
                break;
            }
            spawned++;
        }
        for (uint32_t c = 0; c < 4; c++)
        {
            pair_join(pairs[c]);
        }
        uint64_t const d = kos::clock_now() - t0;
        w_line("w2", spawned * W_ROUNDS * 2u, d);
        sched_report(TAG_W2);
    }

    Atomic<uint32_t, Order::RELAXED> g_w_stop{0};

    void w_spinner(void*)
    {
        while (g_w_stop == 0)
        {
        }
    }

    // W3: an unpinned equal-priority pair, with round-robin spinners on wide masks beside it.
    void w3_wide()
    {
        uint32_t const n = KICKOS_KERNEL_CORES;
        Pair p = {};
        kos::thread::Handle spin[3] = {};
        g_w_stop = 0;
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        uint64_t const t0 = kos::clock_now();
        (void)pair_spawn(0, &p);
        for (uint32_t i = 0; i + 1 < n and i < 3; i++)
        {
            spin[i] = kos::thread::create_caps(w_spinner, nullptr, "w_rr", W_PRIO, nullptr, 0,
                                               KOS_POLICY_RR, W_RR_QUANTUM_NS);
        }
        pair_join(p);
        uint64_t const d = kos::clock_now() - t0;
        g_w_stop = 1;
        w_line("w3", W_ROUNDS * 2u, d);
        sched_report(TAG_W3);
        for (kos::thread::Handle const& h : spin)
        {
            if (h.valid())
            {
                (void)h.join(KOS_TIMEOUT_NONE);
            }
        }
    }

    // The push probe: every core but core 1 holds a spinning hog above the moved thread, which is
    // readied on core 0 behind its hog and pushed when core 1's hog parks.
    Atomic<uint32_t, Order::RELAXED> g_h1_on{0};
    Atomic<uint32_t, Order::RELAXED> g_park_req{0};
    Atomic<uint32_t, Order::RELAXED> g_moved{0};
    constexpr int P_START = 1; // every probe thread: waited once before it begins
    constexpr int P_H1 = 2;    // hog 0 and hog 1: hog 1's release
    constexpr int P_M = 3;     // hog 0 and the moved thread: the moved thread's release

    // Spins until `cell` reads `want`, false once W_PUSH_GIVEUP_NS have passed.
    bool p_await(Atomic<uint32_t, Order::RELAXED>& cell, uint32_t want)
    {
        uint64_t deadline = 0;
        uint32_t spins = 0;
        while (cell != want)
        {
            spins++;
            if (spins < W_SPIN_POLL)
            {
                continue;
            }
            spins = 0;
            uint64_t const now = kos::clock_now();
            if (deadline == 0)
            {
                deadline = now + W_PUSH_GIVEUP_NS;
            }
            else if (now >= deadline)
            {
                return false;
            }
        }
        return true;
    }

    void p_stall(uint32_t round, char const* what)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "  push-probe: STALL round=%u/%u (%s)\n",
                  static_cast<unsigned>(round), static_cast<unsigned>(W_PUSH_ROUNDS), what);
        kickos::emit(s);
    }

    void p_hog0(void*)
    {
        kos_sem_wait(P_START);
        for (uint32_t r = 1; r <= W_PUSH_ROUNDS; r++)
        {
            kos_sem_post(P_H1);
            if (not p_await(g_h1_on, r))
            {
                p_stall(r, "hog 1 never ran");
                break;
            }
            kos_sem_post(P_M);
            g_park_req = r;
            if (not p_await(g_moved, r))
            {
                p_stall(r, "the moved thread was never pushed");
                break;
            }
        }
        g_w_stop = 1;
        kos_sem_post(P_H1);
        kos_sem_post(P_M);
    }

    void p_hog1(void*)
    {
        kos_sem_wait(P_START);
        uint32_t r = 0;
        while (true)
        {
            kos_sem_wait(P_H1);
            if (g_w_stop != 0)
            {
                return;
            }
            r++;
            g_h1_on = r;
            // A stall ends the probe without ever publishing this round's park request.
            while (g_park_req != r and g_w_stop == 0)
            {
            }
        }
    }

    void p_moved(void*)
    {
        kos_sem_wait(P_START);
        uint32_t r = 0;
        while (true)
        {
            kos_sem_wait(P_M);
            if (g_w_stop != 0)
            {
                return;
            }
            r++;
            g_moved = r;
        }
    }

    void p_spinner(void*)
    {
        kos_sem_wait(P_START);
        while (g_w_stop == 0)
        {
        }
    }

    void w4_push()
    {
        uint32_t const n = KICKOS_KERNEL_CORES;
        kos::Semaphore start(0);
        kos_cap_t h1 = KOS_CAP_NONE;
        kos_cap_t m = KOS_CAP_NONE;
        if (kos_sem_create(0, &h1) != 0 or kos_sem_create(0, &m) != 0)
        {
            kickos::emit("  push-probe: SKIP (no semaphore)\n");
            kos_handle_close(h1);
            return;
        }
        kos_cap_grant caps[] = {{start.id(), CH_FULL}, {h1, CH_FULL}, {m, CH_FULL}};
        g_w_stop = 0;
        g_h1_on = 0;
        g_park_req = 0;
        g_moved = 0;
        kos::thread::Handle hs[6] = {};
        uint32_t k = 0;
        hs[k++] = kos::thread::create_caps(p_hog0, nullptr, "p_hog0", W_HOG_PRIO, caps, 3,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                           KOS_TASK_NONE, nullptr, 0, 1u << 0);
        hs[k++] = kos::thread::create_caps(p_hog1, nullptr, "p_hog1", W_HOG_PRIO, caps, 3,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                           KOS_TASK_NONE, nullptr, 0, 1u << 1);
        hs[k++] = kos::thread::create_caps(p_moved, nullptr, "p_moved", W_MOVED_PRIO, caps, 3);
        for (uint32_t c = 2; c < n and k < 6; c++)
        {
            hs[k++] = kos::thread::create_caps(p_spinner, nullptr, "p_spin", W_HOG_PRIO, caps, 3,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                               KOS_TASK_NONE, nullptr, 0, 1u << c);
        }
        kos_handle_close(h1);
        kos_handle_close(m);
        bool all = true;
        for (uint32_t i = 0; i < k; i++)
        {
            if (not hs[i].valid())
            {
                all = false;
            }
        }
        if (all)
        {
            (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
            for (uint32_t i = 0; i < k; i++)
            {
                start.post();
            }
            (void)hs[0].join(KOS_TIMEOUT_NONE);
            sched_report(TAG_PUSH);
        }
        else
        {
            kickos::emit("  push-probe: SKIP (thread pool too small)\n");
            g_w_stop = 1;
            for (uint32_t i = 0; i < k; i++)
            {
                start.post();
            }
            // The two releases are the threads' own now, so a thread still waiting on one is
            // cancelled instead.
            for (uint32_t i = 0; i < k; i++)
            {
                (void)hs[i].kill();
            }
        }
        for (uint32_t i = 0; i < k; i++)
        {
            if (hs[i].valid())
            {
                (void)hs[i].join(KOS_TIMEOUT_NONE);
            }
        }
    }

    // The reseat probe: a thread spinning on core 1, re-masked from this core over and over with
    // core 1 in every mask, so each request is re-seated by that core with the thread running.
    void w4_reseat()
    {
        g_w_stop = 0;
        auto t = kos::thread::create_caps(w_spinner, nullptr, "r_spin", W_MOVED_PRIO, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, 1u << 1);
        if (not t.valid())
        {
            kickos::emit("  reseat-probe: SKIP (thread pool too small)\n");
            return;
        }
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        for (uint32_t r = 0; r < W_RESEAT_ROUNDS; r++)
        {
            uint32_t mask = (1u << 1) | (1u << 0);
            if ((r & 1u) != 0)
            {
                mask = 1u << 1;
            }
            (void)kos_thread_set_affinity(t.id(), mask);
            kos_yield();
        }
        sched_report(TAG_RESEAT);
        g_w_stop = 1;
        (void)t.join(KOS_TIMEOUT_NONE);
    }
}
#endif

// KOS_AUTH_IRQ for the end-to-end span's line mint, KOS_AUTH_MEMORY for the device window
// root reserves and grants, and KOS_AUTH_SYSTEM because main returns and root's exit is a
// shutdown.
KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_IRQ);

int main(int, char**)
{
    kickos::emit("microbenchmark: context-switch throughput (all arches) + per-switch cost\n");
    kickos::emit("+ IRQ-entry latency where a cycle counter exists. Reporter woken by the\n");
    kickos::emit("workload, not a timer. Telemetry OFF for clean numbers.\n");

    int64_t const hz_rc = kos_bench(KOS_BENCH_OP_CYCCNT_HZ, 0, 0);
    uint64_t hz = 0;
    if (hz_rc > 0)
    {
        hz = static_cast<uint64_t>(hz_rc);
    }
    char hzline[96];
    ksnprintf(hzline, sizeof(hzline),
              "cycle counter: %llu Hz (0 = no rate converts a reading; cycles only)\n\n",
              static_cast<unsigned long long>(hz));
    kickos::emit(hzline);

    // BEFORE the reset: the probe's own three brackets are then not in the sweep's numbers.
    (void)kos_bench(KOS_BENCH_OP_LOCK_PROBE, 0, 0);
    (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0); // the phase table below covers the sweep only

    // Each step joins BOTH its peers before the next starts, so two pool slots beside
    // root's carry the whole sweep.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 0);
    }

    // Above KOS_CALL_REG_BYTES kos_call already issues this same trap, so those rows must
    // differ from the register arm by one flat constant, the failed register-form attempt; a
    // ragged difference there means the two arms were not measured alike and the capture is
    // void.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 1);
    }

    // Read the phase table's donate row against the equal-priority rows above.
    measure_callreply(CR_DONATE_SPAN, CR_PRIO + 1u, CR_PRIO, 0);
    (void)kos_bench(KOS_BENCH_OP_PHASE_PRINT, 0, 0); // the kernel writes the table
#if KICKOS_KERNEL_CORES > 1
    sched_report(TAG_CALLREPLY);
#endif
    kickos::emit("\n");

    kos::Semaphore a(0), b(0), gate(0), resume(0);
    g_a = &a;
    g_b = &b;
    g_gate = &gate;
    g_resume = &resume;

    // Players at prio 1 (KICKOS_PRIO_MIN), below root's prio 2, so the reporter preempts
    // them when player_b posts the gate.
    kos_cap_grant acaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}}; // A@1, B@2
    kos_cap_grant bcaps[] = {{a.id(), CH_FULL},
                             {b.id(), CH_FULL},
                             {gate.id(), CH_FULL},
                             {resume.id(), CH_FULL}}; // +gate@3, resume@4
    auto ra = kos::thread::create_caps(player_a, nullptr, "bench_a", 1, acaps, 2, KOS_POLICY_FIFO,
                                       0, /*privileged=*/false, nullptr, 0, 0, nullptr,
                                       KOS_TASK_NONE, nullptr, 0, PLAYER_CORES);
    auto rb = kos::thread::create_caps(player_b, nullptr, "bench_b", 1, bcaps, 4, KOS_POLICY_FIFO,
                                       0, /*privileged=*/false, nullptr, 0, 0, nullptr,
                                       KOS_TASK_NONE, nullptr, 0, PLAYER_CORES);
    if (not ra.valid() or not rb.valid())
    {
        // Do not park here: on a bootloader-handover board a parked app costs a physical
        // button press, which the bounded reporter exists to avoid.
        kickos::emit("bench: FAILED to spawn players (thread pool too small?)\n");
        return 1;
    }

    reporter_loop();
#if KICKOS_KERNEL_CORES > 1
    w2_pairs();
    w3_wide();
    w4_push();
    w4_reseat();
#endif
    kickos::emit("bench: done\n");
    return 0;
}
