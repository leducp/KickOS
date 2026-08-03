// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Context-switch microbenchmark (KICKOS_BENCH builds only). Two equal-priority
// threads ping-pong via semaphores; a higher-priority privileged reporter prints
// throughput (ctx-switches/s via kos::clock_now, works on every arch), per-switch
// cost + IRQ-entry latency (cycles, only where switch.S brackets the swap with a
// counter: armv7m DWT, rxv3 CMTW1, rv32imac rdcycle/MTIME, xtensa CCOUNT; absent
// on M0/sim/frozen-QEMU), and worst-case ISR latency across a masked span.
//
// The reporter is woken by the workload itself, NOT by a timer, so it cannot be
// starved by the players saturating the CPU (see docs/archive/M1_state.md).
// Run telemetry OFF for clean numbers.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

extern "C"
{
    void kickos_bench_switch_reset(void);
    void kickos_bench_switch_report(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
    uint32_t kickos_bench_core_hz(void);
    void kickos_bench_irq_setup(int line);
    uint32_t kickos_bench_irq_once(int line);
    uint32_t kickos_bench_irq_masked_once(int line, uint32_t span_bytes);
    uint32_t kickos_bench_masked_hold_ns(uint32_t span_bytes);
}

namespace
{
    // Any line the bench does not otherwise use (NOT a console TX drain line); the
    // bench enables no other IRQ source, so it only ever fires when we inject it.
    constexpr int BENCH_IRQ_LINE = 20;
    constexpr int IRQ_SAMPLES = 100;
    // Rounds the players run before waking the reporter. Sized so the throughput
    // window is a fraction of a second on fast silicon and a few seconds on a slow
    // M0; the report prints the actual window (ms) so it is self-documenting.
    constexpr uint32_t ROUNDS_PER_REPORT = 20000;

    kos::Semaphore* g_a = nullptr;    // MAIN's caps; the ping-pong sems (reporter side)
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    volatile uint32_t g_rounds = 0;

    // B1 well-known child cap indices (fresh child table => handle == index). MAIN
    // delegates the sems per spawn in a fixed order; the players name them by these.
    constexpr int CH_A = 1;    // ping-pong sem A (delegated first to both players)
    constexpr int CH_B = 2;    // ping-pong sem B (delegated second)
    constexpr int CH_GATE = 3; // reporter-wake gate (delegated third to player_b only)
    constexpr uint8_t CH_FULL = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

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
            // Explicit RMW through a local: '++' on a volatile is deprecated (C++20).
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
    // as low as 2 (nrf51, stm32f103/f302). Root is privileged (DWT/STIR/CCOUNT reads
    // are privileged) and prio KICKOS_PRIO_MIN+1 == 2; the players run at prio 1, so a
    // gate post from player_b preempts straight into the reporter.
    void reporter_loop()
    {
        uint32_t hz = kickos_bench_core_hz();
        kickos_bench_irq_setup(BENCH_IRQ_LINE);
        uint32_t prev_rounds = g_rounds;
        uint64_t prev_ns = kos::clock_now();
        g_a->post(); // kick off the ping-pong

        while (true)
        {
            kickos_bench_switch_reset();
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

            // Worst-case ISR latency, portable term: how long interrupts stay masked
            // across one endpoint-sized (256B) copy span: the interval an ISR waits
            // behind such a syscall critical section. clock_now-based, so it survives a
            // frozen/absent cycle counter (mps2 DWT / sim); the cycle sweep below adds
            // the exception-entry term where a counter exists.
            uint32_t hold_ns = kickos_bench_masked_hold_ns(256);
            if (hold_ns != 0)
            {
                ksnprintf(s, sizeof(s),
                          "  wcase-hold: %u ns masked / 256B endpoint-copy span\n",
                          static_cast<unsigned>(hold_ns));
                kos::print(s);
            }

            // Per-switch cost + IRQ latency only where switch.S bracketed real cycles.
            uint32_t smin, savg, smax, scnt;
            kickos_bench_switch_report(&smin, &savg, &smax, &scnt);
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
                uint32_t c = kickos_bench_irq_once(BENCH_IRQ_LINE);
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

            ksnprintf(s, sizeof(s), "  switch: %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      static_cast<unsigned>(smin), static_cast<unsigned>(savg),
                      static_cast<unsigned>(smax), static_cast<unsigned>(to_ns(smin, hz)),
                      static_cast<unsigned>(to_ns(savg, hz)),
                      static_cast<unsigned>(to_ns(smax, hz)), static_cast<unsigned>(scnt));
            kos::print(s);
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
                    uint32_t c = kickos_bench_irq_masked_once(BENCH_IRQ_LINE, wspans[si]);
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
    // sits directly beside the ctx-switch throughput. Both peers are SPAWNED pool threads:
    // the caller must be a pool thread (a reply cap names its parked caller by ThreadPool
    // slot; the file-static root TCB has no slot). Run once BEFORE the players spawn, so
    // the two threads exit and free their slots and the peak concurrency stays at 2.
    constexpr uint32_t CALLREPLY_REPS = 20000;
    void callreply_server(void*) // caps: E(WAIT)@1, done@2
    {
        unsigned char buf[16];
        struct kos_recv_info info = {0, static_cast<int32_t>(-1)};
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            long n = kos_recv(1, buf, sizeof(buf), &info);
            if (n >= 0 and info.reply_cap >= 0)
            {
                kos_reply(info.reply_cap, buf, static_cast<size_t>(n)); // echo the request back
            }
        }
        kos_sem_post(2);
    }
    void callreply_caller(void*) // caps: E(SIGNAL)@1, done@2
    {
        unsigned char buf[16];
        for (unsigned i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i);
        }
        uint64_t t0 = kos::clock_now();
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            (void)kos_call(1, buf, sizeof(buf), sizeof(buf));
        }
        uint64_t d_ns = kos::clock_now() - t0;

        uint32_t rt_per_s = 0;
        uint32_t ns_per_rt = 0;
        if (d_ns != 0)
        {
            rt_per_s = static_cast<uint32_t>(
                static_cast<uint64_t>(CALLREPLY_REPS) * 1000000000ull / d_ns);
            ns_per_rt = static_cast<uint32_t>(d_ns / CALLREPLY_REPS);
        }
        char s[160];
        ksnprintf(s, sizeof(s),
                  "  call/reply: %u round-trips/s  (%u ns/round-trip over %u calls / %u ms)\n",
                  static_cast<unsigned>(rt_per_s), static_cast<unsigned>(ns_per_rt),
                  static_cast<unsigned>(CALLREPLY_REPS),
                  static_cast<unsigned>(d_ns / 1000000ull));
        kos::print(s);
        kos_sem_post(2);
    }
    void measure_callreply()
    {
        int ep = kos_endpoint_create();
        if (ep < 0)
        {
            kos::print("  call/reply: SKIP (no endpoint)\n");
            return;
        }
        kos::Semaphore done(0);
        kos_cap_grant scaps[] = {{ep, KOS_CAP_WAIT}, {done.id(), CH_FULL}};   // E(WAIT)@1, done@2
        kos_cap_grant ccaps[] = {{ep, KOS_CAP_SIGNAL}, {done.id(), CH_FULL}}; // E(SIGNAL)@1, done@2
        int sv = kos::thread::spawn_caps(callreply_server, nullptr, "cr_srv", 1, scaps, 2);
        int cl = kos::thread::spawn_caps(callreply_caller, nullptr, "cr_cl", 2, ccaps, 2);
        if (sv < 0 or cl < 0)
        {
            // The bench needs both peers or neither can make progress (a lone thread parks
            // forever). Close and skip; never fires where the pool holds 2 (the minimum).
            kos_handle_close(ep);
            kos::print("  call/reply: SKIP (thread pool too small)\n");
            return;
        }
        done.wait(); // caller finished + printed
        done.wait(); // server ran all REPS and exited
        kos_handle_close(ep);
    }
}

int main(int, char**)
{
    kos::print("microbenchmark: context-switch throughput (all arches) + per-switch cost\n");
    kos::print("+ IRQ-entry latency where a cycle counter exists. Reporter woken by the\n");
    kos::print("workload, not a timer. Telemetry OFF for clean numbers.\n\n");

    // One-shot call/reply round-trip (runs before the players spawn, so it reuses a slot).
    measure_callreply();
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
    int ra = kos::thread::spawn_caps(player_a, nullptr, "bench_a", 1, acaps, 2);
    int rb = kos::thread::spawn_caps(player_b, nullptr, "bench_b", 1, bcaps, 3);
    if (ra < 0 or rb < 0)
    {
        kos::print("bench: FAILED to spawn players (thread pool too small?)\n");
        kos::Semaphore park(0);
        while (true)
        {
            park.wait();
        }
    }

    reporter_loop(); // never returns
    return 0;
}
