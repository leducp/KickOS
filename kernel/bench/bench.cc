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

#if defined(__riscv)
// RISC-V cycle source, defined in arch_rv32imac.cc. Default null -> `rdcycle` CSR
// (qemu-virt); a core that traps on it (the ESP32-C6) points it at a free-running MMIO
// counter (CLINT MTIME). Declared at global scope so it keeps C linkage (see switch.S).
extern "C" volatile uint32_t* g_bench_cycle_src;
#endif

namespace
{
    // Per-arch free-running cycle counter for the bench. Returns 0 where the arch has
    // none (Cortex-M0 / sim): there switch.S brackets nothing (scnt stays 0) and the
    // app reports throughput only, so cyccnt() is never actually read on those targets.
#if defined(__riscv)
    inline uint32_t cyccnt()
    {
        if (g_bench_cycle_src != nullptr)
        {
            return *g_bench_cycle_src;
        }
        uint32_t v;
        __asm volatile("rdcycle %0" : "=r"(v));
        return v;
    }
#elif defined(__RX__)
    // CMTW1 free-running counter (7.5 MHz), <<5 (x32) to match switch.S's ICLK-cycle scaling.
    inline uint32_t cyccnt() { return *reinterpret_cast<volatile uint32_t*>(0x00094290u) << 5; }
#elif defined(__XTENSA__)
    inline uint32_t cyccnt()
    {
        uint32_t v;
        __asm volatile("rsr.ccount %0" : "=a"(v)); // LX6 cycle counter @ CPU clock
        return v;
    }
#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
    inline uint32_t cyccnt() { return *reinterpret_cast<volatile uint32_t*>(0xE0001004u); } // DWT CYCCNT
#else
    inline uint32_t cyccnt() { return 0; } // armv6m (no DWT) / sim: throughput-only
#endif

    // IRQ-entry latency: a bench handler timestamps its own entry; kickos_bench_irq_once
    // triggers the line and returns (entry - trigger) cycles.
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
    // Core clock in Hz. The chip backend defines the strong symbol and updates it at
    // PLL bring-up; this weak 0 is the fallback for the sim (no chip clock) -- unused
    // there anyway, since sim reports throughput only (no cycle->ns conversion).
    uint32_t __attribute__((weak)) SystemCoreClock = 0;

    // Attach the bench handler to a spare line + unmask it (call once). The bench
    // app activates no other IRQ source, so any line the console does not own is free.
    void kickos_bench_irq_setup(int line)
    {
        (void)kickos::irq_attach(line, bench_irq_handler, nullptr); // line 20 is always free here
        arch_irq_unmask(line);
    }

    // One IRQ-entry-latency sample in cycles (0 if the line did not fire, or the arch
    // has no cycle counter / no injectable line). Runs in a PRIVILEGED thread.
    uint32_t kickos_bench_irq_once(int line)
    {
        // Re-arm before each inject: some backends mask the logical line on delivery
        // and expect a driver's irq_ack to re-unmask (xtensa's software-doorbell path).
        // The bench's tier-2 handler does not ack, so without this only the FIRST inject
        // would fire (the rest hit the masked-line drop). Idempotent no-op on backends
        // that do not mask on delivery (ARM NVIC / RISC-V).
        arch_irq_unmask(line);
        g_irq_seen = 0;
        uint32_t t0 = cyccnt();
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        *reinterpret_cast<volatile uint32_t*>(0xE000EF00u) = static_cast<uint32_t>(line); // STIR
        __asm volatile("dsb; isb" ::: "memory");
#else
        // RISC-V (PLIC/msip), RX (SWINT2 doorbell), xtensa: raise via the arch inject
        // seam. Where no software-injectable line exists (qemu-virt) it is a no-op and
        // the sample returns 0 -- the switch-cost number is the meaningful one there.
        arch_irq_inject(line);
#endif
        for (uint32_t i = 0; i < 100000u and g_irq_seen == 0; i++)
        {
            __asm volatile("nop");
        }
        if (g_irq_seen == 0)
        {
            return 0; // genuinely did not fire (no injectable line / masked)
        }
        // Fired. "0" is the sentinel for "did not fire", so a real but sub-counter-tick
        // latency (delta==0, e.g. RX's coarse 133 ns CMTW1 tick) must report as 1, not
        // be discarded by the caller's `!= 0` fired-check.
        uint32_t d = g_irq_entry - t0;
        if (d == 0)
        {
            d = 1;
        }
        return d;
    }

    // Switch-entry timestamp, written by the switch handler (switch.S).
    uint32_t g_bench_sw_start = 0;
    // Xtensa only: the windowed exit can't host a call, so switch.S stamps the switch
    // END here and accumulates (end-start) at the NEXT switch entry (a safe call site).
    uint32_t g_bench_sw_end = 0;

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
        // Drop any un-banked xtensa sample so the first switch after a reset only
        // re-primes (banks nothing) -- else the previous window's last switch would
        // leak into this window's min/max (it is banked one switch late by design).
        g_bench_sw_end = 0;
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
