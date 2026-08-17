// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Context-switch microbenchmark (KICKOS_BENCH builds only). Two equal-priority
// threads ping-pong via semaphores; the reporter prints throughput (ctx-switches/s via
// kos::clock_now, works on every arch) plus per-switch cost + IRQ-entry latency (cycles,
// only where switch.S brackets the swap with a counter: armv7m DWT, rxv3 CMTW1,
// rv32imac rdcycle/MTIME, xtensa CCOUNT; absent on M0/sim/frozen-QEMU).
//
// The call/reply round-trip sweep measures the real syscall path, so the copy it times is
// the one under the kernel's own IrqLock. The phase table printed after it breaks that
// round trip's FIXED cost down by kernel phase (kernel/bench/bench.cc).
//
// Every cycle metric here goes through kos_bench, one syscall. The helpers behind it read
// kernel .data and core peripherals, so an app calling them directly would run them at ITS
// privilege, and root is unprivileged on every board: the direct form faulted on all but
// the LX6, which has no privilege ring.
//
// The KERNEL prints the switch line and the phase table (the syscall returns a scalar and
// carries no out-pointer), so this app must run with the kernel console, which is what the
// `bench` config variant pins with kickos_services_none.
//
// The reporter is woken by the workload itself, NOT by a timer, so it cannot be
// starved by the players saturating the CPU (see docs/archive/M1_state.md).
// Run telemetry OFF for clean numbers.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/libc/fmt.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Any line the bench does not otherwise use (NOT a console TX drain line); the
    // bench enables no other IRQ source, so it only ever fires when we inject it.
    constexpr int BENCH_IRQ_LINE = 20;
    constexpr int IRQ_SAMPLES = 100;
    // Rounds the players run before waking the reporter. Sized so the throughput
    // window is a fraction of a second on fast silicon and a few seconds on a slow
    // M0; the report prints the actual window (ms) so it is self-documenting.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;
    // Then main RETURNS, so root's exit reaches kickos_terminate and a board built with
    // KICKOS_SHUTDOWN_TO_BOOTLOADER re-enters its bootloader. A loop here is what used to
    // make every capture on teensy41 and the RP boards cost a physical button press.
    constexpr unsigned THROUGHPUT_REPORTS = 3;

    kos::Semaphore* g_a = nullptr;    // MAIN's caps; the ping-pong sems (reporter side)
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    Atomic<uint32_t, Order::RELAXED> g_rounds{0};

    // B1 well-known child cap indices (fresh child table => handle == index). MAIN
    // delegates the sems per spawn in a fixed order; the players name them by these.
    constexpr int CH_A = 1;    // ping-pong sem A (delegated first to both players)
    constexpr int CH_B = 2;    // ping-pong sem B (delegated second)
    constexpr int CH_GATE = 3; // reporter-wake gate (delegated third to player_b only)
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // A refused op returns -KOS_E*; the cycle ops answer 0 for "did not fire", which is
    // what a refusal has to collapse to so a caller's `!= 0` fired-check still holds.
    uint32_t bench_u32(uint32_t op, uint32_t a0, uint32_t a1)
    {
        int32_t const rc = kos_bench(op, a0, a1);
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

    uint32_t to_ns(uint32_t cyc, uint32_t hz)
    {
        if (hz == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 1000000000ull) / hz);
    }

    // The reporter runs as the ROOT thread (main), not a spawned one, so the bench
    // needs only 2 pool slots (the two players) and fits boards with KICKOS_MAX_THREADS
    // as low as 2 (nrf51, stm32f103/f302). Root is prio KICKOS_PRIO_MIN+1 == 2 and the
    // players run at prio 1, so a gate post from player_b preempts straight into the
    // reporter.
    void reporter_loop(uint32_t hz)
    {
        (void)kos_bench(KOS_BENCH_OP_IRQ_SETUP, BENCH_IRQ_LINE, 0); // no-op if refused
        uint32_t prev_rounds = g_rounds;
        uint64_t prev_ns = kos::clock_now();
        g_a->post(); // kick off the ping-pong

        for (unsigned rep = 0; rep < THROUGHPUT_REPORTS; rep++)
        {
            (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
            g_gate->wait(); // woken by the workload after ROUNDS_PER_REPORT rounds

            // Window = the interval the reporter was blocked on the gate = exactly the
            // players' burst (prev_* is sampled at the END of the previous iteration,
            // just before this gate.wait, so the reporter's own report time is excluded).
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

            // Every %u argument below is cast to `unsigned`: uint32_t is `unsigned
            // long` on the newlib targets and plain `unsigned` on the host and Xtensa,
            // and `unsigned` is 32-bit everywhere KickOS runs. Same convention as
            // user/apps/common/clocksoak.
            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      static_cast<unsigned>(sw_per_s), static_cast<unsigned>(ns_per_sw),
                      static_cast<unsigned>(switches),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kos::print(s);

            // Per-switch cost + IRQ latency only where switch.S bracketed real cycles.
            // The kernel writes the switch line itself and hands back the sample count.
            uint32_t const scnt = bench_u32(KOS_BENCH_OP_SWITCH_PRINT, 0, 0);
            if (scnt == 0)
            {
                // no cycle counter on this arch; throughput is the metric
                prev_rounds = g_rounds;
                prev_ns = kos::clock_now();
                continue;
            }

            uint32_t imin = 0xFFFFFFFFu, imax = 0, icnt = 0;
            uint64_t isum = 0;
            for (int i = 0; i < IRQ_SAMPLES; i++)
            {
                uint32_t c = bench_u32(KOS_BENCH_OP_IRQ_ONCE, BENCH_IRQ_LINE, 0);
                if (c != 0)
                {
                    if (c < imin) { imin = c; }
                    if (c > imax) { imax = c; }
                    isum += c;
                    icnt++;
                }
            }
            uint32_t iavg = 0;
            if (icnt != 0)
            {
                iavg = static_cast<uint32_t>(isum / icnt);
            }
            else
            {
                imin = 0;
            }

            ksnprintf(s, sizeof(s), "  irq:    %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      static_cast<unsigned>(imin), static_cast<unsigned>(iavg),
                      static_cast<unsigned>(imax), static_cast<unsigned>(to_ns(imin, hz)),
                      static_cast<unsigned>(to_ns(iavg, hz)),
                      static_cast<unsigned>(to_ns(imax, hz)), static_cast<unsigned>(icnt));
            kos::print(s);

            // Worst-case inject->entry: raise the line at the START of a masked span,
            // then measure the delay until the ISR runs, swept over span sizes (0 =
            // fixed cost; 256 = endpoint-copy max). Real cycles only where the counter
            // advances (frozen -> ~1 on mps2 DWT, same as the best-case line above).
            static const uint32_t wspans[] = {0, 64, 256, 1024};
            for (unsigned si = 0; si < sizeof(wspans) / sizeof(wspans[0]); si++)
            {
                uint32_t wmin = 0xFFFFFFFFu, wmax = 0, wcnt = 0;
                uint64_t wsum = 0;
                for (int k = 0; k < IRQ_SAMPLES; k++)
                {
                    uint32_t c =
                        bench_u32(KOS_BENCH_OP_IRQ_MASKED_ONCE, BENCH_IRQ_LINE, wspans[si]);
                    if (c != 0)
                    {
                        if (c < wmin) { wmin = c; }
                        if (c > wmax) { wmax = c; }
                        wsum += c;
                        wcnt++;
                    }
                }
                if (wcnt == 0)
                {
                    continue;
                }
                uint32_t wavg = static_cast<uint32_t>(wsum / wcnt);
                ksnprintf(s, sizeof(s),
                          "  wcase-irq[%uB]: %u/%u/%u cyc  %u/%u/%u ns  (inject->entry, n=%u)\n",
                          static_cast<unsigned>(wspans[si]), static_cast<unsigned>(wmin),
                          static_cast<unsigned>(wavg), static_cast<unsigned>(wmax),
                          static_cast<unsigned>(to_ns(wmin, hz)),
                          static_cast<unsigned>(to_ns(wavg, hz)),
                          static_cast<unsigned>(to_ns(wmax, hz)), static_cast<unsigned>(wcnt));
                kos::print(s);
            }

            // Sample prev_* AFTER the report so the next window excludes this report's
            // own IRQ-sampling + print time (the players were paused during it).
            prev_rounds = g_rounds;
            prev_ns = kos::clock_now();
        }
    }

    // Call/reply round-trip: the same 2-switches-per-round handoff as the sem ping-pong
    // above (caller -> server on the call, server -> caller on the reply), so the number
    // sits directly beside the ctx-switch throughput. Both peers are SPAWNED so the number
    // is a worker-to-worker figure and not root's.
    constexpr uint32_t CALLREPLY_REPS = 20000;

    // Both peers run ABOVE root (prio KICKOS_PRIO_MIN + 1 == 2). A peer MUST outrank root:
    // it posts `done` as its last act but reaches EXITED only afterwards, and root preempting
    // it on that post leaves the peer READY, holding a slot ThreadPool::alloc cannot reclaim
    // when the next sweep step spawns. Where the pool is 3 slots that spawn is -KOS_ENOMEM.
    constexpr uint8_t CR_PRIO = 4;

    // The payload one sweep step measures. Written by measure_callreply BEFORE it spawns
    // either peer, so neither thread races the write.
    Atomic<uint32_t, Order::RELAXED> g_cr_len{16};

    // Non-zero when the caller was spawned ABOVE the server, which is the only shape that
    // makes endpoint_call take the D1 donation branch: it is guarded on caller prio strictly
    // greater than server prio, so an equal-priority sweep never executes it and never prices
    // it. Same write discipline as g_cr_len.
    Atomic<uint32_t, Order::RELAXED> g_cr_donating{0};

    void callreply_server(void*) // caps: E(WAIT)@1, done@2
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            long n = kos_recv(1, buf, sizeof(buf), &info);
            if (n < 0 or info.reply_cap == KOS_CAP_NONE)
            {
                // A reply-less message is the stop sentinel measure_callreply sends to drain
                // a server whose peer never spawned; a parked receiver pins its own WAIT cap,
                // so nothing but a message can end that park.
                break;
            }
            kos_reply(info.reply_cap, buf, static_cast<size_t>(n)); // echo the request back
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
        uint64_t t0 = kos::clock_now();
        uint32_t reps = 0;
        while (reps < CALLREPLY_REPS)
        {
            // In place: the request goes out of this buffer and the reply lands back in it.
            if (kos_call(1, buf, len, len) < 0)
            {
                break;
            }
            reps++;
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
            char s[192];
            ksnprintf(
                s, sizeof(s),
                "  call/reply: %u B  %u ns/round-trip  (%u round-trips/s over %u calls / %u ms)%s\n",
                static_cast<unsigned>(len), static_cast<unsigned>(ns_per_rt),
                static_cast<unsigned>(rt_per_s), static_cast<unsigned>(CALLREPLY_REPS),
                static_cast<unsigned>(d_ns / 1000000ull), shape);
            kos::print(s);
        }
        kos_sem_post(2);
    }
    // caller_prio strictly above server_prio drives the donation branch; equal leaves it
    // unexecuted. Both must stay ABOVE root for the slot-reclaim reason CR_PRIO documents.
    void measure_callreply(uint32_t len, uint8_t caller_prio, uint8_t server_prio)
    {
        g_cr_len = len;
        uint32_t donating = 0;
        if (caller_prio > server_prio)
        {
            donating = 1;
        }
        g_cr_donating = donating;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            kos::print("  call/reply: SKIP (no endpoint)\n");
            return;
        }
        kos::Semaphore done(0);
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}, {done.id(), CH_FULL}};   // E(WAIT)@1, done@2
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}, {done.id(), CH_FULL}}; // E(SIGNAL)@1, done@2
        auto sv = kos::thread::spawn_caps(callreply_server, nullptr, "cr_srv", server_prio, scaps, 2);
        auto cl = kos::thread::spawn_caps(callreply_caller, nullptr, "cr_cl", caller_prio, ccaps, 2);
        if (not sv.valid() or not cl.valid())
        {
            // Neither peer can make progress alone, and a lone one holds its slot for the whole
            // run unless it is drained HERE: the server has to be sent the sentinel, and the
            // caller wakes -KOS_EPIPE only once recv_holders reaches 0, which is this close.
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
            kos::print("  call/reply: SKIP (thread pool too small)\n");
            return;
        }
        done.wait(); // caller finished + printed
        done.wait(); // server ran all REPS and exited
        kos_handle_close(ep);
    }

    // The payload sizes the round-trip sweep walks, up to the ceiling a call can carry.
    // The slope across them is TWICE the per-byte cost of the endpoint copy, because a
    // round trip copies the request in and the reply back, both under one IrqLock.
    constexpr uint32_t CR_SPANS[] = {8, 16, 32, 64, 128, 256};

    // The payload the donating step measures. One size is enough: D1 raises the server's
    // effective priority and that cost does not scale with the message, so a sweep would
    // re-measure the copy slope the equal-priority sweep already gives.
    constexpr uint32_t CR_DONATE_SPAN = 32;
}

int main(int, char**)
{
    kos::print("microbenchmark: context-switch throughput (all arches) + per-switch cost\n");
    kos::print("+ IRQ-entry latency where a cycle counter exists. Reporter woken by the\n");
    kos::print("workload, not a timer. Telemetry OFF for clean numbers.\n");

    // Stated, not left to be derived: every cycle figure below is in core clocks, and a
    // capture that does not carry the clock cannot be converted to time after the fact.
    uint32_t const hz = bench_u32(KOS_BENCH_OP_CORE_HZ, 0, 0);
    char hzline[80];
    ksnprintf(hzline, sizeof(hzline), "core clock: %u Hz (0 = the chip backend does not say)\n\n",
              static_cast<unsigned>(hz));
    kos::print(hzline);

    (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0); // the phase table below covers the sweep only

    // Runs before the players spawn, and each step joins BOTH its peers before the next
    // starts, so two pool slots beside root's carry the whole sweep.
    for (unsigned i = 0; i < sizeof(CR_SPANS) / sizeof(CR_SPANS[0]); i++)
    {
        measure_callreply(CR_SPANS[i], CR_PRIO, CR_PRIO);
    }

    // The same round trip with the caller one level ABOVE the server, which is what makes
    // endpoint_call run its D1 donation and endpoint_reply shed it. Read the phase table's
    // donate row against the equal-priority rows above: with equal peers that row reports no
    // samples at all, so the branch is unpriced without this step. Both peers stay above root.
    measure_callreply(CR_DONATE_SPAN, CR_PRIO + 1u, CR_PRIO);
    (void)kos_bench(KOS_BENCH_OP_PHASE_PRINT, 0, 0); // the kernel writes the table
    kos::print("\n");

    kos::Semaphore a(0), b(0), gate(0);
    g_a = &a;
    g_b = &b;
    g_gate = &gate;

    // Players at prio 1 (KICKOS_PRIO_MIN): below root's prio 2 so the reporter (root)
    // preempts them when player_b posts the gate. Only two spawned threads: fits the
    // smallest pool (KICKOS_MAX_THREADS == 2).
    kos_cap_grant acaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}};                    // A@1, B@2
    kos_cap_grant bcaps[] = {{a.id(), CH_FULL}, {b.id(), CH_FULL}, {gate.id(), CH_FULL}}; // +gate@3
    auto ra = kos::thread::spawn_caps(player_a, nullptr, "bench_a", 1, acaps, 2);
    auto rb = kos::thread::spawn_caps(player_b, nullptr, "bench_b", 1, bcaps, 3);
    if (not ra.valid() or not rb.valid())
    {
        // Returns rather than parks: a park here would cost the same physical button press
        // on a bootloader-handover board that the bounded reporter exists to avoid.
        kos::print("bench: FAILED to spawn players (thread pool too small?)\n");
        return 1;
    }

    reporter_loop(hz);
    kos::print("bench: done\n");
    return 0;
}
