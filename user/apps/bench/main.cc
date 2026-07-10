// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Context-switch microbenchmark (KICKOS_BENCH builds only). Two equal-priority
// threads ping-pong via semaphores; every wait/post handoff forces one scheduler
// switch. A higher-priority privileged reporter prints two things:
//
//   throughput -- context switches/second, measured over a burst of N rounds with
//     the monotonic clock (kos::clock_now). Needs no cycle counter, so it is the
//     UNIFORM metric: it works on every arch, including Cortex-M0 and the sim.
//   per-switch cost + IRQ-entry latency -- in CPU cycles, only where the arch has a
//     cycle counter that switch.S brackets the swap with (armv7m DWT, rxv3 CMTW1,
//     rv32imac rdcycle/MTIME, xtensa CCOUNT). Absent (scnt==0) on M0/sim/frozen-QEMU.
//
// The reporter is woken by the workload itself (player_b posts a gate every N rounds),
// NOT by a timer -- so it cannot be starved by the players saturating the CPU (a real
// tickless-timer starvation under 100%-CPU zero-idle load; see M1_state.md). Numbers
// are comparable across arches and to ourselves once M2 adds an MPU reprogram to the
// switch. Run telemetry OFF for clean numbers.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

extern "C"
{
    void kickos_bench_switch_reset(void);
    void kickos_bench_switch_report(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
    uint32_t kickos_bench_core_hz(void);
    void kickos_bench_irq_setup(int line);
    uint32_t kickos_bench_irq_once(int line);
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

    kos::Semaphore* g_a = nullptr;
    kos::Semaphore* g_b = nullptr;
    kos::Semaphore* g_gate = nullptr;
    volatile uint32_t g_rounds = 0;

    void player_a(void*)
    {
        while (true)
        {
            g_a->wait();
            g_b->post();
        }
    }
    void player_b(void*)
    {
        while (true)
        {
            g_b->wait();
            g_a->post();
            // Deterministically wake the reporter every N rounds -- a semaphore post
            // (direct reschedule to the higher-prio reporter), never a timer, so it
            // cannot be starved by this CPU-bound ping-pong.
            if ((++g_rounds % ROUNDS_PER_REPORT) == 0)
            {
                g_gate->post();
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

            char s[160];
            ksnprintf(s, sizeof(s),
                      "  throughput: %u ctx-sw/s  (%u ns/sw avg over %u switches / %u ms)\n",
                      sw_per_s, ns_per_sw, static_cast<uint32_t>(switches),
                      static_cast<uint32_t>(d_ns / 1000000ull));
            kos::print(s);

            // Per-switch cost + IRQ latency only where switch.S bracketed real cycles.
            uint32_t smin, savg, smax, scnt;
            kickos_bench_switch_report(&smin, &savg, &smax, &scnt);
            if (scnt == 0)
            {
                // no cycle counter on this arch -- throughput is the metric
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
                      smin, savg, smax, to_ns(smin, hz), to_ns(savg, hz), to_ns(smax, hz), scnt);
            kos::print(s);
            ksnprintf(s, sizeof(s), "  irq:    %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      imin, iavg, imax, to_ns(imin, hz), to_ns(iavg, hz), to_ns(imax, hz), icnt);
            kos::print(s);

            // Sample prev_* AFTER the report so the next window excludes this report's
            // own IRQ-sampling + print time (the players were paused during it).
            prev_rounds = g_rounds;
            prev_ns = kos::clock_now();
        }
    }
}

int main(int, char**)
{
    kos::print("microbenchmark: context-switch throughput (all arches) + per-switch cost\n");
    kos::print("+ IRQ-entry latency where a cycle counter exists. Reporter woken by the\n");
    kos::print("workload, not a timer. Telemetry OFF for clean numbers.\n\n");

    kos::Semaphore a(0), b(0), gate(0);
    g_a = &a;
    g_b = &b;
    g_gate = &gate;

    // Players at prio 1 (KICKOS_PRIO_MIN) -- below root's prio 2 so the reporter (root)
    // preempts them when player_b posts the gate. Only two spawned threads: fits the
    // smallest pool (KICKOS_MAX_THREADS == 2).
    int ra = kos::thread::spawn(player_a, nullptr, "bench_a", 1);
    int rb = kos::thread::spawn(player_b, nullptr, "bench_b", 1);
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
