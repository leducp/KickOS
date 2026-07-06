// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Microbenchmark (KICKOS_BENCH builds only): bare context-switch cost + IRQ-entry
// latency, in DWT cycles (converted to ns via SystemCoreClock). Two equal-priority
// threads ping-pong forever; each wait/post handoff forces one PendSV switch, which
// switch.S brackets with CYCCNT. A higher-priority privileged reporter samples the
// accumulated switch stats every interval and, between samples, measures IRQ-entry
// latency by software-triggering a spare line (STIR) whose handler timestamps its
// own entry. Reports periodically so it is easy to read on the console regardless
// of when you attach. Run telemetry OFF for clean numbers. Numbers are comparable
// to other RTOSes and to ourselves once M2 adds an MPU reprogram to the switch.
// DWT is cycle-accurate on silicon; a QEMU/mps2 DWT may be frozen (reports 0).

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
    // Any line the bench does not otherwise use. NOT the console TX drain line
    // (K64F UART0 = 31, XMC USIC0 SR0 = 84); the bench enables no other IRQ source,
    // so this line only ever fires when we STIR it.
    constexpr int BENCH_IRQ_LINE = 20;
    constexpr int IRQ_SAMPLES = 100;

    kos::Semaphore* g_a = nullptr;
    kos::Semaphore* g_b = nullptr;

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

    // Privileged (DWT CYCCNT + STIR are privileged) and higher priority than the
    // players, so it preempts them to report and to run the IRQ samples uninterrupted.
    void reporter(void*)
    {
        uint32_t hz = kickos_bench_core_hz();
        kickos_bench_irq_setup(BENCH_IRQ_LINE);
        g_a->post(); // kick off the ping-pong

        while (true)
        {
            kickos_bench_switch_reset();
            kos::sleep_ns(500000000ull); // players accumulate switches while we sleep

            uint32_t smin, savg, smax, scnt;
            kickos_bench_switch_report(&smin, &savg, &smax, &scnt);

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

            char s[128];
            ksnprintf(s, sizeof(s), "  switch: %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      smin, savg, smax, to_ns(smin, hz), to_ns(savg, hz), to_ns(smax, hz), scnt);
            kos::print(s);
            ksnprintf(s, sizeof(s), "  irq:    %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                      imin, iavg, imax, to_ns(imin, hz), to_ns(iavg, hz), to_ns(imax, hz), icnt);
            kos::print(s);
        }
    }
}

int main(int, char**)
{
    kos::print("microbenchmark: context switch + IRQ-entry latency (DWT cycles).\n");
    kos::print("reports every 0.5s. DWT is cycle-accurate on silicon; frozen (0) on QEMU.\n\n");

    kos::Semaphore a(0), b(0);
    g_a = &a;
    g_b = &b;

    kos::thread::spawn(player_a, nullptr, "bench_a", 10);
    kos::thread::spawn(player_b, nullptr, "bench_b", 10);
    kos::thread::spawn(reporter, nullptr, "bench_rep", 15, KOS_POLICY_FIFO, 0, true);

    kos::Semaphore park(0);
    while (true)
    {
        park.wait();
    }
}
