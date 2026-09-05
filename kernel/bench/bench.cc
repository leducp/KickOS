// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cycle-accurate microbenchmark state (KICKOS_BENCH builds only). The SWITCH accumulator is
// fed from the arch switch handler (switch.S): the measured window is the register + FP +
// CONTROL save/restore, NOT the hardware exception entry, which is IRQ-entry latency. The
// PHASE accumulator is fed by the brackets in the syscall/scheduler/timer paths
// (<kickos/bench.h>).
//
// MIN is the statistic to read: the XMC4800's DWT is documented unreliable on that silicon
// (chip_xmc4800.cc), and a glitched counter read can only inflate a delta, never push it
// below the true minimum.
//
// The KERNEL prints both tables, from thread context and outside any IrqLock.

#include <kickos/irq_route.h>
#include <kickos/bench.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/arch/arch.h>
#include <kickos/sys/atomic.h>

#include <stdint.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;
    using kickos::bench_cyccnt;

    // IRQ-entry latency: the handler stamps its own entry here.
    constinit Atomic<uint32_t, Order::RELAXED> g_irq_entry = 0;
    constinit Atomic<uint32_t, Order::RELAXED> g_irq_seen = 0;

    // Nothing but the cycle stamp belongs in here: any work added between entry and the seen
    // flag inflates every sample.
    void bench_irq_handler(void*)
    {
        g_irq_entry = bench_cyccnt();
        g_irq_seen = 1;
    }

    // Masked-span body: a byte copy across these models the endpoint copy under IrqLock
    // (bounded by KOS_EP_MSG_MAX). volatile so it is neither elided nor hoisted out of the
    // masked window.
    constexpr uint32_t BENCH_LAT_SPAN_MAX = 1024;
    constinit volatile uint8_t g_lat_src[BENCH_LAT_SPAN_MAX] = {0};
    constinit volatile uint8_t g_lat_dst[BENCH_LAT_SPAN_MAX] = {0};

    // Set the line pending. On ARM a direct STIR write, which works while PRIMASK holds the
    // span masked; elsewhere the arch inject seam, a no-op where no line is software
    // injectable and the sample then reads 0.
    inline void bench_irq_raise(int line)
    {
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        *reinterpret_cast<volatile uint32_t*>(0xE000EF00u) = static_cast<uint32_t>(line); // STIR
        __asm volatile("dsb; isb" ::: "memory");
#else
        arch_irq_inject(line);
#endif
    }

    // --- Switch accumulator ---------------------------------------------------
    constinit uint32_t g_sw_min = 0xFFFFFFFFu;
    constinit uint32_t g_sw_max = 0;
    constinit uint32_t g_sw_count = 0;
    constinit uint64_t g_sw_sum = 0;

    // --- Phase accumulators ---------------------------------------------------
    struct PhaseAcc
    {
        uint32_t min;
        uint32_t max;
        uint32_t count;
        uint64_t sum;
    };
    constinit PhaseAcc g_phase[kickos::PH_COUNT] = {};

    // Padded to one width: kvsnprintf implements no field width (lib/libc/fmt.cc), so a
    // "%-14s" here would print the flag and the digits literally.
    constexpr char const* PHASE_NAME[kickos::PH_COUNT] = {
        "NULL            ", "NEST            ", "CALL_TOTAL      ", "CALL_VALIDATE   ",
        "CALL_LOCKED     ", "CALL_RESOLVE    ", "CALL_PEEK       ", "CALL_PROBE      ",
        "CALL_POP        ", "CALL_COPY       ", "CALL_MINT       ",
        "CALL_MINT_CAP   ", "CALL_MINT_INFO  ", "CALL_DONATE     ", "CALL_PARK       ",
        "CALL_WAKE       ", "CALL_RESUME     ",
        "CALL_SLOW_TOTAL ", "CALL_SLOW_LOCKED",
        "CALL_SLOW_DONATE", "CALL_SLOW_PARK  ",
        "RECV_LOCKED     ", "RECV_RESOLVE    ", "RECV_SCAN       ", "RECV_PARK       ",
        "REPLY_TOTAL     ", "REPLY_VALIDATE  ",
        "REPLY_LOCKED    ", "REPLY_LOOKUP    ", "REPLY_COPY      ", "REPLY_FUNNEL    ",
        "REPLY_WAKE      ", "WAKE_UNPARK     ", "PICK_NEXT       ", "SWITCH_TO       ",
        "SWITCH_BOOK     ", "MPU_APPLY       ", "MPU_COMMIT      ", "KTIME_REARM     ",
        "ARCH_SWITCH     "};
    static_assert(sizeof(PHASE_NAME) / sizeof(PHASE_NAME[0]) == kickos::PH_COUNT,
                  "one name per phase, in enum order");
}

extern "C"
{
    // Core clock in Hz, defined and maintained by the chip backend at PLL bring-up.
    // The sim has no chip: arch/sim/system_core_clock_default.cc carries its 0.
    extern uint32_t SystemCoreClock;

    // Switch-entry timestamp, written by the switch handler (switch.S).
    constinit uint32_t g_bench_sw_start = 0;
    // Xtensa only: the windowed exit can't host a call, so switch.S stamps the switch
    // END here and accumulates (end-start) at the NEXT switch entry (a safe call site).
    constinit uint32_t g_bench_sw_end = 0;

    // The arch's deferred MPU commit runs from the switch epilogue, below the kernel
    // headers, so it reaches the accumulator through this the way switch.S reaches
    // kickos_bench_switch_done.
    void kickos_bench_mpu_commit(uint32_t delta)
    {
        kickos::bench_phase_add(kickos::PH_MPU_COMMIT, delta);
    }

    void kickos_bench_switch_done(uint32_t delta)
    {
        if (delta < g_sw_min)
        {
            g_sw_min = delta;
        }
        if (delta > g_sw_max)
        {
            g_sw_max = delta;
        }
        g_sw_sum += delta;
        g_sw_count++;
    }
}

namespace
{
    // 0 where the chip backend publishes no clock (the sim), which prints as 0 ns.
    uint32_t cyc_to_ns(uint32_t cyc)
    {
        if (SystemCoreClock == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 1000000000ull)
                                     / SystemCoreClock);
    }
}

namespace kickos
{
    void bench_phase_add(uint32_t phase, uint32_t delta)
    {
        if (phase >= PH_COUNT)
        {
            return;
        }
        PhaseAcc& a = g_phase[phase];
        if (a.count == 0 or delta < a.min)
        {
            a.min = delta;
        }
        if (delta > a.max)
        {
            a.max = delta;
        }
        a.sum += delta;
        a.count++;
    }

    void bench_reset()
    {
        // Locked, unlike the two prints: this runs in thread context with interrupts on, and
        // the switch handler writes the same accumulator from the switch tail.
        IrqLock lock;
        g_sw_min = 0xFFFFFFFFu;
        g_sw_max = 0;
        g_sw_sum = 0;
        g_sw_count = 0;
        // Drop any un-banked xtensa sample, which is banked one switch late: else the previous
        // window's last switch leaks into this window's min/max.
        g_bench_sw_end = 0;
        for (uint32_t i = 0; i < PH_COUNT; i++)
        {
            g_phase[i] = PhaseAcc{};
        }
    }

    uint32_t bench_switch_print()
    {
        uint32_t const c = g_sw_count;
        uint32_t min = 0;
        uint32_t avg = 0;
        if (c != 0)
        {
            min = g_sw_min;
            avg = static_cast<uint32_t>(g_sw_sum / c);
        }
        kprintf("  switch: %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n",
                static_cast<unsigned>(min), static_cast<unsigned>(avg),
                static_cast<unsigned>(g_sw_max), static_cast<unsigned>(cyc_to_ns(min)),
                static_cast<unsigned>(cyc_to_ns(avg)), static_cast<unsigned>(cyc_to_ns(g_sw_max)),
                static_cast<unsigned>(c));
        return c;
    }

    void bench_phase_print()
    {
        kprintf("  phase table (cycles; leaf -= NULL, composite -= k*NEST for k nested):\n");
        for (uint32_t i = 0; i < PH_COUNT; i++)
        {
            PhaseAcc const& a = g_phase[i];
            uint32_t min = 0;
            uint32_t avg = 0;
            if (a.count != 0)
            {
                min = a.min;
                avg = static_cast<uint32_t>(a.sum / a.count);
            }
            kprintf("    %s %u/%u/%u  n=%u\n", PHASE_NAME[i], static_cast<unsigned>(min),
                    static_cast<unsigned>(avg), static_cast<unsigned>(a.max),
                    static_cast<unsigned>(a.count));
        }
    }

    uint32_t bench_core_hz() { return SystemCoreClock; }

    // Attach the bench handler to a spare line and unmask it. Call once.
    void bench_irq_setup(int line)
    {
        (void)irq_attach(line, bench_irq_handler, nullptr);
        irq_line_op(line, LineOp::CLEAR); // discard pre-arm garbage (latch-and-coalesce contract)
        irq_line_op(line, LineOp::UNMASK);
    }

    // One IRQ-entry-latency sample in cycles (0 if the line did not fire, or the arch
    // has no cycle counter / no injectable line).
    uint32_t bench_irq_once(int line)
    {
        // Re-arm before each inject: some backends mask the logical line on delivery and
        // expect a driver's irq_ack to re-unmask (xtensa's software-doorbell path). The bench
        // handler does not ack, so without this only the FIRST inject would fire. No-op on
        // backends that do not mask on delivery (ARM NVIC / RISC-V).
        irq_line_op(line, LineOp::UNMASK);
        g_irq_seen = 0;
        uint32_t t0 = bench_cyccnt();
        bench_irq_raise(line);
        for (uint32_t i = 0; i < 100000u and g_irq_seen == 0; i++)
        {
            __asm volatile("nop");
        }
        if (g_irq_seen == 0)
        {
            return 0; // genuinely did not fire (no injectable line / masked)
        }
        // 0 is the sentinel for "did not fire", so a real but sub-counter-tick latency
        // (delta==0, e.g. RX's coarse 133 ns CMTW1 tick) must report as 1 rather than be
        // discarded by the caller's `!= 0` fired-check.
        uint32_t d = g_irq_entry - t0;
        if (d == 0)
        {
            d = 1;
        }
        return d;
    }

    // WORST-case ISR-entry latency: raise the line at the START of a masked span, hold
    // interrupts off across a bounded body (span_bytes of the endpoint-copy model), then
    // release. Returns inject-to-entry cycles (span hold + exception entry), or 0 where the
    // line is not injectable. The mask is the SAME arch_irq_save/restore seam kickos::IrqLock
    // wraps. Frozen-counter arches (mps2 DWT / sim) read ~1, exactly as the best-case line
    // does.
    uint32_t bench_irq_masked_once(int line, uint32_t span_bytes)
    {
        if (span_bytes > BENCH_LAT_SPAN_MAX)
        {
            span_bytes = BENCH_LAT_SPAN_MAX;
        }
        irq_line_op(line, LineOp::UNMASK);
        g_irq_seen = 0;
        arch_irq_state_t st = arch_irq_save(); // span begins; a raised IRQ is held off
        uint32_t t0 = bench_cyccnt();
        bench_irq_raise(line);                 // pending now; cannot fire until restore
        for (uint32_t i = 0; i < span_bytes; i++)
        {
            g_lat_dst[i] = g_lat_src[i];
        }
        arch_irq_restore(st);                  // unmask -> pending IRQ runs, stamps entry
        for (uint32_t i = 0; i < 100000u and g_irq_seen == 0; i++)
        {
            __asm volatile("nop");
        }
        if (g_irq_seen == 0)
        {
            return 0;
        }
        uint32_t d = g_irq_entry - t0;
        if (d == 0)
        {
            d = 1;
        }
        return d;
    }
}
