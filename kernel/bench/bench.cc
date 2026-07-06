// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cycle-accurate context-switch microbenchmark (KICKOS_BENCH builds only). The
// armv7m PendSV handler brackets the switch body with two DWT CYCCNT reads
// (switch.S) and calls kickos_bench_switch_done(delta) with the elapsed cycles;
// the `bench` app resets, drives N switches, and reports min/avg/max. Compiled
// out entirely in normal builds. Gives a bare context-switch number to compare
// against other RTOSes and against ourselves once M2 adds an MPU reprogram to the
// switch. The measured window is the SOFTWARE switch (register + FP + CONTROL
// save/restore), not the hardware exception entry (that is IRQ-entry latency).

#include <kickos/irq.h>
#include <kickos/arch/arch.h>

#include <stdint.h>

namespace
{
    inline uint32_t cyccnt() { return *reinterpret_cast<volatile uint32_t*>(0xE0001004u); }

    // IRQ-entry latency: a bench handler timestamps its own entry; kickos_bench_irq_once
    // triggers the line via STIR and returns (entry - trigger) cycles.
    volatile uint32_t g_irq_entry = 0;
    volatile uint32_t g_irq_seen = 0;

    void bench_irq_handler(void*)
    {
        g_irq_entry = cyccnt();
        g_irq_seen = 1;
    }
}

extern "C"
{
    extern uint32_t SystemCoreClock; // chip backend; used to turn cycles into ns

    // Attach the bench handler to a spare line + unmask it (call once). The bench
    // app activates no other IRQ source, so any line the console does not own is free.
    void kickos_bench_irq_setup(int line)
    {
        kickos::irq_attach(line, bench_irq_handler, nullptr);
        arch_irq_unmask(line);
    }

    // One IRQ-entry-latency sample in cycles (0 if the IRQ did not fire). MUST run
    // in a PRIVILEGED thread (DWT CYCCNT + STIR are privileged registers).
    uint32_t kickos_bench_irq_once(int line)
    {
        g_irq_seen = 0;
        uint32_t t0 = cyccnt();
        *reinterpret_cast<volatile uint32_t*>(0xE000EF00u) = static_cast<uint32_t>(line); // STIR
        __asm volatile("dsb; isb" ::: "memory");
        for (uint32_t i = 0; i < 100000u and g_irq_seen == 0; i++)
        {
            __asm volatile("nop");
        }
        if (g_irq_seen == 0)
        {
            return 0;
        }
        return g_irq_entry - t0;
    }

    // Switch-entry CYCCNT timestamp, written by PendSV_Handler (switch.S).
    uint32_t g_bench_sw_start = 0;

    uint32_t kickos_bench_core_hz(void) { return SystemCoreClock; }

    namespace
    {
        uint32_t s_min = 0xFFFFFFFFu;
        uint32_t s_max = 0;
        uint32_t s_count = 0;
        uint64_t s_sum = 0;
    }

    void kickos_bench_switch_done(uint32_t delta)
    {
        if (delta < s_min)
        {
            s_min = delta;
        }
        if (delta > s_max)
        {
            s_max = delta;
        }
        s_sum += delta;
        s_count++;
    }

    void kickos_bench_switch_reset(void)
    {
        s_min = 0xFFFFFFFFu;
        s_max = 0;
        s_sum = 0;
        s_count = 0;
    }

    // avg is 0 when no switch was recorded (min stays at its sentinel then too).
    void kickos_bench_switch_report(uint32_t* out_min, uint32_t* out_avg,
                                    uint32_t* out_max, uint32_t* out_count)
    {
        uint32_t c = s_count;
        *out_count = c;
        *out_max = s_max;
        if (c != 0)
        {
            *out_min = s_min;
            *out_avg = static_cast<uint32_t>(s_sum / c);
        }
        else
        {
            *out_min = 0;
            *out_avg = 0;
        }
    }
}
