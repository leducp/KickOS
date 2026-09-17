// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Context-switch microbenchmark (KICKOS_BENCH builds only). Two equal-priority
// threads ping-pong via semaphores; the reporter prints throughput (ctx-switches/s via
// kos::clock_now, works on every arch) plus per-switch cost + IRQ-entry latency (cycles,
// where switch.S brackets the swap with a counter: armv7m DWT, rxv3 CMTW1, rv32imac
// rdcycle/MTIME, xtensa CCOUNT). Whether such a cycle converts to time is a separate fact,
// KOS_BENCH_OP_CYCCNT_HZ, and a 0 from it leaves every ns column off.
//
// The call/reply sweep times the endpoint copy under the kernel's own IrqLock. The phase
// table printed after it breaks that round trip's FIXED cost down by kernel phase
// (kernel/bench/bench.cc).
//
// Every cycle metric goes through kos_bench, one syscall: the helpers behind it read kernel
// .data and core peripherals, and root is unprivileged on every board except the LX6, which
// has no privilege ring.
//
// The app's own lines go through kickos::emit, which reaches a published console driver
// over IPC and falls back to the kernel console when index 0 is empty. kos_print alone
// would be dropped outright once a service list publishes the UART (sys/emit.h).
//
// The switch line and the phase table are printed by the KERNEL from inside kos_bench,
// straight at the kernel console, so an instrumented run wants kickos_services_none,
// which the `bench` config variant pins.

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
    // Per PASS, and the sweep runs two passes per kernel core.
    constexpr int E2E_SAMPLES = 25;
    // Two waiter placements per raiser placement: the raiser's own core, and the next one.
    constexpr uint32_t E2E_PAIRS_PER_CORE = 2;
    // What e2e_sweep runs, and the denominator the kernel echoes beside the raises it let
    // through.
    constexpr uint32_t E2E_SWEEP_PASSES =
        KICKOS_KERNEL_CORES * E2E_PAIRS_PER_CORE * static_cast<uint32_t>(E2E_SAMPLES);
    // Yields this thread will spend waiting for the waiter to park, and then for it to close.
    constexpr int E2E_TRIES = 2000;
    // The instrument's own tail, measured once per run: the return to userspace, the device
    // read, and the trap back in.
    constexpr int E2E_TARE_SAMPLES = 64;
    // An MPU rounds a grant up to what it can describe and the rounded window must still lie
    // inside the block that was reserved.
    constexpr uint32_t E2E_DEV_BYTES = 4096;
    // Rounds the players run before waking the reporter: a throughput window of a
    // fraction of a second on fast silicon, a few seconds on a slow M0.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;
    // Rounds the doorbell probe runs per core per window.
    constexpr uint32_t DOORBELL_ROUNDS = KOS_BENCH_ROUNDS_MAX;
    // An upper bound on the walk alone: a core set is a 32-bit mask everywhere KickOS runs.
    constexpr uint32_t CORE_WALK_MAX = 32;
    // Bounded so main RETURNS: root's exit reaches kickos_terminate, and a board built
    // with KICKOS_SHUTDOWN_TO_BOOTLOADER re-enters its bootloader instead of needing a
    // physical button press for the next capture.
    constexpr unsigned THROUGHPUT_REPORTS = 3;

    kos::Semaphore* g_a = nullptr;    // MAIN's caps
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    Atomic<uint32_t, Order::RELAXED> g_rounds{0};

    // Child cap indices (a fresh child table makes handle == index). MAIN delegates the
    // sems per spawn in this order.
    constexpr int CH_A = 1;    // ping-pong sem A
    constexpr int CH_B = 2;    // ping-pong sem B
    constexpr int CH_GATE = 3; // reporter-wake gate, delegated to player_b only
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

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
    void player_b(void*) // caps: A@1, B@2, gate@3
    {
        while (true)
        {
            kos_sem_wait(CH_B);
            kos_sem_post(CH_A);
            uint32_t const round = g_rounds + 1;
            g_rounds = round;
            if ((round % ROUNDS_PER_REPORT) == 0)
            {
                kos_sem_post(CH_GATE);
            }
        }
    }

    // --- the end-to-end span ---------------------------------------------------
    // BOTH ENDS ARE SPAWNED THREADS OF ROOT'S OWN TASK, so root can place either.
    // Root itself holds no handle on itself and so cannot be the raiser.
    // The waiter holds the line, so nothing else in the image may.
    constexpr int CH_E2E_IRQ = 1;   // waiter: the claimed line, WAIT only
    constexpr int CH_E2E_READY = 2; // waiter: posted once, after the tare, before the first park
    constexpr int CH_E2E_GO = 1;    // raiser: one post per pass
    constexpr int CH_E2E_PASS = 2;  // raiser: posted when a pass has run its samples

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

    void e2e_waiter(void*)
    {
        auto irq = kos::Irq::adopt(CH_E2E_IRQ);
        irq.attach(); // this thread serves the line
        // Under an MPU reachability is per THREAD, so root's own grant of this block does not
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
            if (irq.wait() != 0)
            {
                break; // cancelled, or the binding died: nothing left to wake for
            }
            // THE CLOSING STAMP IS THE NEXT INSTRUCTION AFTER THIS READ, so the span carries
            // the read and one trap and nothing else of this thread's.
            dev[1] = dev[0];
            (void)kos_bench(KOS_BENCH_OP_E2E_CLOSE, 0, 0);
            g_e2e_done = g_e2e_done + 1;
        }
    }

    // Ranks BELOW the waiter, so every raise runs to the waiter's close before this thread is
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

    // Root claims the line and hands a WAIT-only copy to the waiter, which outranks both root
    // and the raiser.
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
        int const crc = kos_irq_claim(BENCH_E2E_LINE, KOS_IRQ_EDGE, &irq);
        if (crc != 0)
        {
            char cs[96];
            ksnprintf(cs, sizeof(cs), "  e2e: SKIP (line %u claim refused, rc=%d)\n",
                      static_cast<unsigned>(BENCH_E2E_LINE), static_cast<int>(crc));
            kickos::emit(cs);
            return false;
        }
        // The claim leaves the line MASKED and the waiter's first arm happens after the ready
        // handshake, so a raise before that would land on a masked line and be discarded.
        kos_irq_ack(irq);
        if (kos_sem_create(0, &g_e2e_ready) != 0)
        {
            kos_handle_close(irq);
            return false;
        }
        kos_cap_grant caps[] = {{irq, KOS_CAP_WAIT}, {g_e2e_ready, CH_FULL}};
        auto w = kos::thread::create_caps(e2e_waiter, nullptr, "e2ewait", 15, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                          KOS_AUTH_MEMORY);
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

    // The raiser on each core in turn, and for each of those the waiter on that same core and
    // then on the next one. THE SAME-CORE PASS IS WHAT CONSTRUCTS A LOCAL WAKE, and it has to
    // be constructed on both delivery models: where the line follows its injector every
    // same-core pass is local and every next-core pass is cross, and where the controller picks
    // a fixed core one waiter placement of each kind matches it.
    void e2e_sweep()
    {
        if (g_e2e_tid == KOS_THREAD_NONE or g_e2e_rid == KOS_THREAD_NONE)
        {
            return;
        }
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            for (uint32_t d = 0; d < E2E_PAIRS_PER_CORE; d++)
            {
                uint32_t const w = (c + d) % KICKOS_KERNEL_CORES;
                (void)kos_thread_set_affinity(g_e2e_rid, 1u << c); // -KOS_ENOSYS at one core
                (void)kos_thread_set_affinity(g_e2e_tid, 1u << w);
                kos_sem_post(g_e2e_go);
                kos_sem_wait(g_e2e_pass);
            }
        }
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
        uint32_t prev_rounds = g_rounds;
        uint64_t prev_ns = kos::clock_now();
        g_a->post();

        for (unsigned rep = 0; rep < THROUGHPUT_REPORTS; rep++)
        {
            (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
            g_gate->wait();

            // The window is the players' burst alone: prev_* is sampled at the END of the
            // previous iteration, excluding the reporter's own report time.
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

            // Every %u argument is cast to `unsigned`: uint32_t is `unsigned long` on the
            // newlib targets and plain `unsigned` on the host and Xtensa, and `unsigned` is
            // 32-bit everywhere KickOS runs.
            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      static_cast<unsigned>(sw_per_s), static_cast<unsigned>(ns_per_sw),
                      static_cast<unsigned>(switches),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kickos::emit(s);

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

            // Cycles only where switch.S bracketed them. The kernel writes one line per
            // named distribution and hands back the SWITCH sample count.
            uint32_t const scnt = bench_u32(KOS_BENCH_OP_DIST_PRINT, 0, 0);
            if (scnt == 0)
            {
                // no cycle counter on this arch; throughput is the metric
                prev_rounds = g_rounds;
                prev_ns = kos::clock_now();
                continue;
            }

            // Every row below is a NAMED DISTRIBUTION the kernel fills and prints. The
            // worst-case slot is ONE accumulator re-reported per span, so each row's n is
            // that span's.
            (void)kos_bench(KOS_BENCH_OP_IRQ_SWEEP, IRQ_SAMPLES, 0);
            uint32_t const nspans = bench_u32(KOS_BENCH_OP_WCASE_SPANS, 0, 0);
            for (uint32_t si = 0; si < nspans; si++)
            {
                (void)kos_bench(KOS_BENCH_OP_IRQ_WCASE, si, IRQ_SAMPLES);
            }
            e2e_sweep();
            (void)kos_bench(KOS_BENCH_OP_E2E_PRINT, E2E_SWEEP_PASSES, 0);

            // AFTER the report: the next window excludes this report's own sampling
            // and print time.
            prev_rounds = g_rounds;
            prev_ns = kos::clock_now();
        }
        e2e_stop();
    }

    // Call/reply round-trip: the same 2-switches-per-round handoff as the sem ping-pong
    // above. Both peers are SPAWNED, so the figure is worker-to-worker and not root's.
    constexpr uint32_t CALLREPLY_REPS = 20000;

    // A peer MUST outrank root (prio KICKOS_PRIO_MIN + 1 == 2): it posts `done` as its last
    // act but reaches EXITED only afterwards, and root preempting it on that post leaves the
    // peer READY, holding a slot ThreadPool::alloc cannot reclaim for the next sweep step.
    // On a 3-slot pool that spawn is -KOS_ENOMEM.
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
        // Carried from one pass to the next: the answer to request k rides the syscall that
        // receives request k+1, which is the whole of what this loop is here to measure.
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
        // The terminal answer, which no receive follows: the last caller is parked on it.
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

    // THE BOUNDED-RUN INVARIANT.
    // A distribution bucket counts in uint32_t (kernel/include/kickos/bench_hist.h), so a run
    // posting 2^32 samples to one slot wraps and every percentile read off it is wrong with
    // nothing on the wire saying so. Each call/reply rep and each ping-pong round drives two
    // switches and a switch posts one sample to each slot on the switch path, so this product
    // is the busiest slot to an order of magnitude; the margin below carries the rest.
    constexpr uint64_t BENCH_BUSIEST_SLOT =
        2ull * (static_cast<uint64_t>(CALLREPLY_REPS)
                    * (2ull * (sizeof(CR_SPANS) / sizeof(CR_SPANS[0])) + 1ull)
                + static_cast<uint64_t>(THROUGHPUT_REPORTS) * ROUNDS_PER_REPORT);
    static_assert(BENCH_BUSIEST_SLOT < (1ull << 31),
                  "these rep counts drive a distribution slot towards a 32-bit wrap; the "
                  "percentiles under it would be wrong and no row would say so");
}

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

    // The generic arm of EVERY span, so one run holds both sides of the fastpath comparison
    // and its own control. Above KOS_CALL_REG_BYTES kos_call already issues this same trap,
    // so those rows must differ by one flat constant, the failed register-form attempt; a
    // ragged difference there says the two arms were not measured under the same conditions
    // and the whole capture is void.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO, 1);
    }

    // Read the phase table's donate row against the equal-priority rows above.
    measure_callreply(CR_DONATE_SPAN, CR_PRIO + 1u, CR_PRIO, 0);
    (void)kos_bench(KOS_BENCH_OP_PHASE_PRINT, 0, 0); // the kernel writes the table
    kickos::emit("\n");

    kos::Semaphore a(0), b(0), gate(0);
    g_a = &a;
    g_b = &b;
    g_gate = &gate;

    // Players at prio 1 (KICKOS_PRIO_MIN), below root's prio 2, so the reporter preempts
    // them when player_b posts the gate.
    kos_cap_grant acaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}};                    // A@1, B@2
    kos_cap_grant bcaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}, {gate.id(), CH_FULL}}; // +gate@3
    auto ra = kos::thread::create_caps(player_a, nullptr, "bench_a", 1, acaps, 2);
    auto rb = kos::thread::create_caps(player_b, nullptr, "bench_b", 1, bcaps, 3);
    if (not ra.valid() or not rb.valid())
    {
        // Do not park here: on a bootloader-handover board a parked app costs a physical
        // button press, which the bounded reporter exists to avoid.
        kickos::emit("bench: FAILED to spawn players (thread pool too small?)\n");
        return 1;
    }

    reporter_loop();
    kickos::emit("bench: done\n");
    return 0;
}
