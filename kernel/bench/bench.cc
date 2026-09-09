// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cycle-accurate microbenchmark state (KICKOS_BENCH builds only). The SWITCH accumulator is
// fed from the arch switch handler (switch.S). Its window is the SOFTWARE register save, the
// swap and the software register restore; what the hardware stacks and unstacks on its own is
// outside it, that being IRQ-entry latency, and so is the deferred MPU commit, which owns a
// phase row. THE THREE BACKENDS DO NOT ALL BRACKET ALL THREE PARTS: armv7m and rv32imac do,
// rxv3 stops at the swap, and the LX6 stamps its end before the windowed exit reloads the
// incoming caller. The PHASE accumulator is fed by the brackets in the syscall/scheduler/
// timer paths (<kickos/bench.h>).
//
// A part whose cycle counter glitches declares KICKOS_CHIP_CYCCNT_GLITCHES (its
// chip_limits.h; today the XMC4800's DWT alone, chip_xmc4800.cc). There MIN is the statistic,
// a glitched read being able only to inflate a delta and never push one below the true
// minimum. Everywhere else a minimum hides the tail, and the headline is p50/p99/max.
//
// The counter's RATE is a chip fact of its own, KICKOS_CHIP_CYCCNT_HZ, falling back on
// SystemCoreClock. A 0 there says nothing converts a reading into time, and the nanosecond
// columns are then not printed.
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

// The sim has no chip, so this header need not exist.
#if defined(__has_include) && __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

#if defined(KICKOS_CHIP_CYCCNT_GLITCHES) && KICKOS_CHIP_CYCCNT_GLITCHES
#define BENCH_SWITCH_HEADLINE_MIN 1
#define BENCH_SWITCH_FMT "  switch: %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n"
#define BENCH_SWITCH_FMT_CYC "  switch: %u/%u/%u cyc  (min/avg/max, n=%u)\n"
#else
#define BENCH_SWITCH_HEADLINE_MIN 0
#define BENCH_SWITCH_FMT "  switch: %u/%u/%u cyc  %u/%u/%u ns  (p50/p99/max, n=%u)\n"
#define BENCH_SWITCH_FMT_CYC "  switch: %u/%u/%u cyc  (p50/p99/max, n=%u)\n"
#endif

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

#if !BENCH_SWITCH_HEADLINE_MIN
    // Log-linear buckets for the switch percentiles: the three bits under the value's most
    // significant one, so a bucket spans at most an eighth of its own octave. A reported
    // percentile is its bucket's LOW edge and therefore a FLOOR, and two runs are comparable
    // only to that resolution. 168 buckets reach 2^23; a bigger delta saturates the last one.
    // No phase histogram: it would have to run inside bench_phase_add, whose cost every
    // enclosing composite is charged for k times over.
    constexpr uint32_t SW_HIST = 168;
    constinit uint32_t g_sw_hist[SW_HIST] = {};

    // No __builtin_clz: rv32imac has no clz instruction, so it would resolve to a libgcc
    // call, and this runs from a trap-path root the stack-descent gate measures.
    constexpr uint32_t sw_bucket(uint32_t v)
    {
        if (v < 8u)
        {
            return v;
        }
        uint32_t e = 3u;
        while ((v >> (e + 1u)) != 0u)
        {
            e++;
        }
        uint32_t const idx = 8u + (e - 3u) * 8u + ((v >> (e - 3u)) & 7u);
        if (idx >= SW_HIST)
        {
            return SW_HIST - 1u;
        }
        return idx;
    }

    constexpr uint32_t sw_bucket_low(uint32_t idx)
    {
        if (idx < 8u)
        {
            return idx;
        }
        uint32_t const e = 3u + (idx - 8u) / 8u;
        return (8u + ((idx - 8u) % 8u)) << (e - 3u);
    }

    // A wrong edge moves every percentile and no run would say so, hence a compile-time
    // control: an edge never above its own sample, within an eighth of it, and monotone.
    constexpr bool sw_hist_sane()
    {
        uint32_t prev = 0;
        for (uint32_t v = 0; v < 200000u; v += 13u)
        {
            uint32_t const b = sw_bucket(v);
            if (b < prev)
            {
                return false;
            }
            prev = b;
            uint32_t const lo = sw_bucket_low(b);
            if (lo > v)
            {
                return false;
            }
            if ((v - lo) > ((lo / 8u) + 1u))
            {
                return false;
            }
        }
        return true;
    }
    static_assert(sw_hist_sane(), "a switch-percentile bucket must contain its own sample");
    static_assert(sw_bucket(1u << 23) == SW_HIST - 1u,
                  "a delta past the last bucket edge saturates rather than wrapping");

    // The lowest bucket edge at or above the num/den-th sample.
    uint32_t sw_percentile(uint32_t num, uint32_t den)
    {
        if (g_sw_count == 0)
        {
            return 0;
        }
        uint64_t const target =
            (static_cast<uint64_t>(g_sw_count) * num + den - 1ull) / den;
        uint64_t seen = 0;
        for (uint32_t i = 0; i < SW_HIST; i++)
        {
            seen += g_sw_hist[i];
            if (seen >= target)
            {
                return sw_bucket_low(i);
            }
        }
        return g_sw_max;
    }
#endif

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
    // rv32imac only. Its window spans a trap entry and a trap exit, and the exit leaves no
    // register free at the mret, so switch.S banks the halves: g_bench_sw_half carries the
    // save and swap (non-zero means a switch opened the window), g_bench_sw_rstart stamps
    // the restore, and g_bench_sw_pend holds the completed delta until the NEXT switch,
    // which is the next site that can host a call.
    constinit uint32_t g_bench_sw_half = 0;
    constinit uint32_t g_bench_sw_rstart = 0;
    constinit uint32_t g_bench_sw_pend = 0;

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
#if !BENCH_SWITCH_HEADLINE_MIN
        g_sw_hist[sw_bucket(delta)]++;
#endif
    }
}

namespace
{
    uint32_t cyccnt_hz()
    {
#if defined(KICKOS_CHIP_CYCCNT_HZ)
        return KICKOS_CHIP_CYCCNT_HZ;
#else
        return SystemCoreClock;
#endif
    }

    // Callers reach this only past a non-zero cyccnt_hz(); the guard is the divisor's own.
    uint32_t cyc_to_ns(uint32_t cyc)
    {
        uint32_t const hz = cyccnt_hz();
        if (hz == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 1000000000ull) / hz);
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
        // Drop any un-banked sample, banked one switch late on the xtensa and the rv32: else
        // the previous window's last switch leaks into this window's min/max.
        g_bench_sw_end = 0;
        g_bench_sw_half = 0;
        g_bench_sw_pend = 0;
#if !BENCH_SWITCH_HEADLINE_MIN
        for (uint32_t i = 0; i < SW_HIST; i++)
        {
            g_sw_hist[i] = 0;
        }
#endif
        for (uint32_t i = 0; i < PH_COUNT; i++)
        {
            g_phase[i] = PhaseAcc{};
        }
    }

    uint32_t bench_switch_print()
    {
        uint32_t const c = g_sw_count;
#if BENCH_SWITCH_HEADLINE_MIN
        uint32_t lo = 0;
        uint32_t mid = 0;
        if (c != 0)
        {
            lo = g_sw_min;
            mid = static_cast<uint32_t>(g_sw_sum / c);
        }
#else
        uint32_t const lo = sw_percentile(1, 2);
        uint32_t const mid = sw_percentile(99, 100);
#endif
        // The statistic names go in the LITERAL and not in a %s argument: a ninth argument
        // spills to the outgoing stack area, and this frame is on the SYSPRIV chain the
        // stack-descent gate measures against a red zone.
        if (cyccnt_hz() == 0)
        {
            kprintf(BENCH_SWITCH_FMT_CYC, static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                    static_cast<unsigned>(g_sw_max), static_cast<unsigned>(c));
            return c;
        }
        kprintf(BENCH_SWITCH_FMT, static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                static_cast<unsigned>(g_sw_max), static_cast<unsigned>(cyc_to_ns(lo)),
                static_cast<unsigned>(cyc_to_ns(mid)), static_cast<unsigned>(cyc_to_ns(g_sw_max)),
                static_cast<unsigned>(c));
        return c;
    }

    void bench_phase_print()
    {
        kprintf("  phase table (cyc avg/max, min last and a floor; leaf -= NULL,"
                " composite -= NULL + k*(NEST-NULL)):\n");
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
            kprintf("    %s %u/%u  min=%u  n=%u\n", PHASE_NAME[i], static_cast<unsigned>(avg),
                    static_cast<unsigned>(a.max), static_cast<unsigned>(min),
                    static_cast<unsigned>(a.count));
        }
    }

    uint32_t bench_cyccnt_hz() { return cyccnt_hz(); }

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
