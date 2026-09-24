// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-core benchmark accumulators and histograms (KICKOS_BENCH only).
// Percentiles use bucket lower bounds. See docs/reference/bench.md for
// switch timing boundaries and counter limitations.

#include <kickos/irq_route.h>
#include <atomic>

#include <kickos/bench.h>
#include <kickos/bench_hist.h>
#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>
#include <kickos/arch/arch.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/errno.h>

#include <stddef.h>
#include <stdint.h>

// The sim has no chip, so this header need not exist.
#if defined(__has_include) && __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// Keep statistic labels in format literals: an extra argument increases the
// stack depth on the SYSPRIV path. Below the sample floor, report max instead of p99.
#if defined(KICKOS_CHIP_CYCCNT_GLITCHES) && KICKOS_CHIP_CYCCNT_GLITCHES
#define BENCH_HEADLINE_MIN 1
#define BENCH_DIST_FMT(label) "  " label " %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n"
#define BENCH_DIST_FMT_CYC(label) "  " label " %u/%u/%u cyc  (min/avg/max, n=%u)\n"
#define BENCH_DIST_FMT_CNT(label) "  " label " %u/%u/%u  (min/avg/max, n=%u)\n"
#define BENCH_E2E_FMT(label) "  " label " %u/%u/%u ns  (min/avg/max, n=%u)\n"
// The min/avg/max format does not depend on sample count.
#define BENCH_DIST_FMT_MAX(label) BENCH_DIST_FMT(label)
#define BENCH_DIST_FMT_CYC_MAX(label) BENCH_DIST_FMT_CYC(label)
#define BENCH_DIST_FMT_CNT_MAX(label) BENCH_DIST_FMT_CNT(label)
#define BENCH_E2E_FMT_MAX(label) BENCH_E2E_FMT(label)
#else
#define BENCH_HEADLINE_MIN 0
#define BENCH_DIST_FMT(label) "  " label " %u/%u/%u cyc  %u/%u/%u ns  (p50/p99/max, n=%u)\n"
#define BENCH_DIST_FMT_CYC(label) "  " label " %u/%u/%u cyc  (p50/p99/max, n=%u)\n"
#define BENCH_DIST_FMT_CNT(label) "  " label " %u/%u/%u  (p50/p99/max, n=%u)\n"
#define BENCH_E2E_FMT(label) "  " label " %u/%u/%u ns  (p50/p99/max, n=%u)\n"
#define BENCH_DIST_FMT_MAX(label) "  " label " %u/%u/%u cyc  %u/%u/%u ns  (p50/max/max, n=%u)\n"
#define BENCH_DIST_FMT_CYC_MAX(label) "  " label " %u/%u/%u cyc  (p50/max/max, n=%u)\n"
#define BENCH_DIST_FMT_CNT_MAX(label) "  " label " %u/%u/%u  (p50/max/max, n=%u)\n"
#define BENCH_E2E_FMT_MAX(label) "  " label " %u/%u/%u ns  (p50/max/max, n=%u)\n"
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;
    using kickos::bench_cyccnt;

    // The release store to g_irq_seen publishes the handler timestamp and core ID.
    constinit Atomic<uint32_t, Order::RELAXED> g_irq_entry = 0;
    constinit Atomic<uint32_t, Order::RELAXED> g_irq_core = kickos::BENCH_CORE_NONE;
    constinit Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_irq_seen = 0;

    // Read the timestamp before any bookkeeping to avoid inflating samples.
    void bench_irq_handler(void*)
    {
        g_irq_entry = bench_cyccnt();
        g_irq_core = kickos_kernel_core();
        g_irq_seen = 1;
    }

    // End-to-end IRQ span. Accept a sample only if a span is open, the closer
    // is the armed waiter, its switch count advanced, and an ISR ran. Other
    // samples increment g_e2e_dropped. Armed and parked are distinct states.
    enum E2eMode : uint32_t
    {
        E2E_IDLE = 0,
        E2E_ARMED,
        E2E_PARKED,
        E2E_RAISED,
        E2E_TARED
    };

    // State publishes the ordinary data fields. On SMP, release/acquire orders
    // ARMED metadata, PARKED readiness, and RAISED timestamp t0.
    // Transitions use loads and stores, not compare-and-swap. The benchmark
    // semaphore handshake serializes one armer, raiser, closer, and reporter;
    // concurrent users are unsupported (docs/reference/bench.md).
    // Single-core builds need only compiler ordering against interrupts.
#if KICKOS_KERNEL_CORES > 1
    using E2eState = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;
#else
    using E2eState = Atomic<uint32_t, Order::RELAXED>;
#endif
    constinit E2eState g_e2e_mode = E2E_IDLE;
    constinit kickos::Thread* g_e2e_waiter = nullptr;
    constinit uint32_t g_e2e_epoch = 0;
    // The state publishes this 64-bit timestamp; kickos::Atomic supports only 32-bit fields.
    constinit uint64_t g_e2e_t0 = 0;
    // Written by the closer and read by the reporter.
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_closed = 0;
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_dropped = 0;
    // Count requested passes, including those that never parked and closed no sample.
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_raised = 0;

    // Model an endpoint copy under IrqLock. Volatile prevents removal or movement
    // out of the measured interval.
    constexpr uint32_t BENCH_LAT_SPAN_MAX = 1024;
    constinit volatile uint8_t g_lat_src[BENCH_LAT_SPAN_MAX] = {0};
    constinit volatile uint8_t g_lat_dst[BENCH_LAT_SPAN_MAX] = {0};

    // STIR can pend an ARM interrupt while PRIMASK is set. Other targets use
    // arch_irq_inject, which may be unsupported. Include the method in the report.
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
#define BENCH_RAISE_NAME "stir"
#else
#define BENCH_RAISE_NAME "inject"
#endif

    inline void bench_irq_raise(int line)
    {
#if defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        *reinterpret_cast<volatile uint32_t*>(0xE000EF00u) = static_cast<uint32_t>(line); // STIR
        __asm volatile("dsb; isb" ::: "memory");
#else
        arch_irq_inject(line);
#endif
    }

// Report the handler core: none for no delivery, mixed for multiple cores.
#define BENCH_IRQ_PROBE_FMT(label)                                                             \
    "  " label ": line=%u raise=" BENCH_RAISE_NAME " on=%u raised=%u hcore=%u foreign=%u"      \
    " win=%u\n"
#define BENCH_IRQ_PROBE_FMT_NONE(label)                                                        \
    "  " label ": line=%u raise=" BENCH_RAISE_NAME " on=%u raised=%u hcore=none foreign=%u"    \
    " win=%u\n"
#define BENCH_IRQ_PROBE_FMT_MIXED(label)                                                       \
    "  " label ": line=%u raise=" BENCH_RAISE_NAME " on=%u raised=%u hcore=mixed foreign=%u"   \
    " win=%u\n"

    // IRQ selected by bench_irq_setup; negative until setup succeeds.
    constinit int g_irq_line = -1;

    // The masked spans the worst-case sweep walks: 0 is the fixed cost and 256 is
    // KOS_EP_MSG_MAX, the endpoint copy this models.
    constexpr uint32_t WCASE_SPANS[] = {0, 64, 256, 1024};

    constexpr bool wcase_spans_fit(uint32_t max)
    {
        for (uint32_t i = 0; i < sizeof(WCASE_SPANS) / sizeof(WCASE_SPANS[0]); i++)
        {
            if (WCASE_SPANS[i] > max)
            {
                return false;
            }
        }
        return true;
    }
    // Reject spans that irq_masked_once would truncate.
    static_assert(wcase_spans_fit(BENCH_LAT_SPAN_MAX),
                  "a WCASE_SPANS entry exceeds BENCH_LAT_SPAN_MAX");

    // count marks valid data. Saturated samples enter no statistics or histogram.
    struct Acc
    {
        uint32_t min;
        uint32_t max;
        uint32_t count;
        uint32_t sat;
        uint64_t sum;
    };

    // Reject samples wider than 32 bits. Keep inline to avoid an extra stack frame
    // on the switch tail, which trap_redzone checks.
    inline __attribute__((always_inline)) bool acc_add(Acc& a, kickos::BenchTick delta)
    {
#if KICKOS_BENCH_TICK_BITS == 64
        if (delta > 0xFFFFFFFFull)
        {
            a.sat++;
            return false;
        }
#endif
        uint32_t const d = static_cast<uint32_t>(delta);
        if (a.count == 0 or d < a.min)
        {
            a.min = d;
        }
        if (d > a.max)
        {
            a.max = d;
        }
        a.sum += d;
        a.count++;
        return true;
    }

    // End-to-end overhead without an interrupt: return to userspace, read the device,
    // and trap back in. One waiter supplies all samples.
    constinit Acc g_e2e_tare = {};

#if KICKOS_BENCH_TICK_BITS == 64
    // Test saturation with one valid and one oversized delta in a separate
    // accumulator. Keep it static to avoid increasing the measured stack depth.
    constinit Acc g_sat_probe = {};
    constexpr uint32_t SAT_PROBE_FITS = 1000u;
    // The low 32 bits exceed the valid sample, so accidental truncation changes max.
    constexpr uint64_t SAT_PROBE_WIDE = 0x100002000ull;
#endif

    // Merge count last: min uses it to detect an empty accumulator.
    // Merge sat even when the source has no valid samples.
    void acc_merge(Acc& into, Acc const& from)
    {
        into.sat += from.sat;
        if (from.count == 0)
        {
            return;
        }
        if (into.count == 0 or from.min < into.min)
        {
            into.min = from.min;
        }
        if (from.max > into.max)
        {
            into.max = from.max;
        }
        into.sum += from.sum;
        into.count += from.count;
    }

    // Pad names here because kvsnprintf does not support field widths.
    constexpr char const* PHASE_NAME[kickos::PH_COUNT] = {
        "NULL            ", "NEST            ", "NEST_LOCK       ",
        "CALL_TOTAL      ", "CALL_VALIDATE   ",
        "CALL_LOCKED     ", "CALL_RESOLVE    ", "CALL_PEEK       ", "CALL_PROBE      ",
        "CALL_POP        ", "CALL_COPY       ", "CALL_MINT       ",
        "CALL_MINT_CAP   ", "CALL_MINT_INFO  ", "CALL_DONATE     ", "CALL_PARK       ",
        "CALL_WAKE       ", "CALL_RESUME     ",
        "CALL_SLOW_TOTAL ", "CALL_SLOW_LOCKED",
        "CALL_SLOW_DONATE", "CALL_SLOW_PARK  ",
        "RECV_LOCKED     ", "RECV_RESOLVE    ", "RECV_SCAN       ", "RECV_PARK       ",
        "REPLY_TOTAL     ", "REPLY_VALIDATE  ",
        "REPLY_LOCKED    ", "REPLY_LOOKUP    ", "REPLY_COPY      ", "REPLY_FUNNEL    ",
        "REPLY_WAKE      ", "REPLY_RECV_TOTAL", "REPLY_RECV_TAIL ",
        "WAKE_UNPARK     ", "PICK_NEXT       ", "SWITCH_TO       ",
        "SWITCH_BOOK     ", "MPU_APPLY       ", "MPU_COMMIT      ", "REENT_SEAT      ",
        "KTIME_REARM     ",
        "ARCH_SWITCH     "};
    static_assert(sizeof(PHASE_NAME) / sizeof(PHASE_NAME[0]) == kickos::PH_COUNT,
                  "one name per phase, in enum order");

    // Format pairs in BenchDist order. The sample count selects p99 or max.
    struct StatFmt
    {
        char const* p99;
        char const* max;
    };
    struct DistFmt
    {
        StatFmt ns;
        StatFmt cyc;
    };
#define BENCH_DIST_ENTRY(label)                                   \
    {{BENCH_DIST_FMT(label), BENCH_DIST_FMT_MAX(label)},          \
     {BENCH_DIST_FMT_CYC(label), BENCH_DIST_FMT_CYC_MAX(label)}}
#define BENCH_DIST_ENTRY_NONE() {{nullptr, nullptr}, {nullptr, nullptr}}
// A slot carrying a COUNT. The absent nanosecond pair is what dist_print_fmt reads to keep a
// count out of the cycles-to-nanoseconds conversion.
#define BENCH_DIST_ENTRY_CNT(label)                                   \
    {{nullptr, nullptr},                                              \
     {BENCH_DIST_FMT_CNT(label), BENCH_DIST_FMT_CNT_MAX(label)}}
    constexpr DistFmt DIST_FMT[kickos::BD_COUNT] = {
        BENCH_DIST_ENTRY("switch:   "),
        BENCH_DIST_ENTRY("lock-hold:"),
#if KICKOS_KERNEL_CORES > 1
        BENCH_DIST_ENTRY("lock-wait:"),
        BENCH_DIST_ENTRY_CNT("draw-retry:"),
        BENCH_DIST_ENTRY_CNT("draw-queue:"),
        BENCH_DIST_ENTRY_CNT("resched-ask:"),
        BENCH_DIST_ENTRY_CNT("resched-take:"),
        BENCH_DIST_ENTRY("doorbell: "),
#endif
        BENCH_DIST_ENTRY("irq:      "),
        // WCASE_FMT selects the span label without adding a printf argument.
        BENCH_DIST_ENTRY_NONE(),
        // End-to-end rows use nanoseconds because per-core cycle counters are not
        // synchronized. Their labels are in E2E_FMT.
        BENCH_DIST_ENTRY_NONE(),
#if KICKOS_KERNEL_CORES > 1
        BENCH_DIST_ENTRY_NONE(),
#endif
    };
    static_assert(sizeof(DIST_FMT) / sizeof(DIST_FMT[0]) == kickos::BD_COUNT,
                  "one label per distribution, in enum order");

    constexpr DistFmt WCASE_FMT[] = {
        BENCH_DIST_ENTRY("wcase-irq[0B]:   "),
        BENCH_DIST_ENTRY("wcase-irq[64B]:  "),
        BENCH_DIST_ENTRY("wcase-irq[256B]: "),
        BENCH_DIST_ENTRY("wcase-irq[1024B]:"),
    };
    static_assert(sizeof(WCASE_FMT) / sizeof(WCASE_FMT[0])
                      == sizeof(WCASE_SPANS) / sizeof(WCASE_SPANS[0]),
                  "one label per masked span, in span order");

    constexpr StatFmt E2E_FMT_LOCAL = {BENCH_E2E_FMT("e2e-local:"),
                                       BENCH_E2E_FMT_MAX("e2e-local:")};
#if KICKOS_KERNEL_CORES > 1
    constexpr StatFmt E2E_FMT_CROSS = {BENCH_E2E_FMT("e2e-cross:"),
                                       BENCH_E2E_FMT_MAX("e2e-cross:")};
#endif

    // Require 1000 samples for p99, leaving ten samples in the top percent.
#if BENCH_HEADLINE_MIN
    constexpr uint32_t DIST_P99_FLOOR = 0; // Average, not a percentile.
#else
    constexpr uint32_t DIST_P99_FLOOR = 1000;
#endif

    char const* stat_fmt(StatFmt const& f, uint32_t n)
    {
        if (n < DIST_P99_FLOOR)
        {
            return f.max;
        }
        return f.p99;
    }

    // Ordinary reports exclude slots populated by explicit sweeps.
    constexpr uint32_t DIST_SWEPT_FIRST = kickos::BD_IRQ_ENTRY;

    // The benchmark bounds sample counts to fit 32-bit buckets; see bench_hist.h.
    struct DistSlot
    {
        Acc acc;
#if !BENCH_HEADLINE_MIN
        uint32_t hist[kickos::BENCH_HIST];
#endif
    };

    struct KICKOS_BENCH_PERCORE_ALIGNED BenchRow
    {
        DistSlot dist[kickos::BD_COUNT];
        Acc phase[kickos::PH_COUNT];
        // Publish the maximum and its release address together. lock_site_gen is odd
        // during an update. All three fields are atomic because readers can overlap writes.
        // Use std::atomic for uintptr_t; kickos::Atomic is limited to four bytes.
        std::atomic<uintptr_t> lock_site;
        std::atomic<uint32_t> lock_site_max;
        std::atomic<uint32_t> lock_site_gen;
    };

#if KICKOS_KERNEL_CORES > 1
    static_assert(sizeof(BenchRow) % KICKOS_BENCH_CACHE_LINE == 0,
                  "a row shorter than a line would share one with the next writer");
#endif

    // Bound retries if a writer is paused during publication.
    constexpr uint32_t LOCK_SITE_READ_TRIES = 8u;

    constinit BenchRow g_row[KICKOS_KERNEL_CORES] = {};

    BenchRow& row() { return g_row[kickos_kernel_core()]; }

#if !BENCH_HEADLINE_MIN
    static_assert(sizeof(BenchRow) % sizeof(uint32_t) == 0,
                  "the stride between two rows must be a whole number of buckets");
    constexpr uint32_t DIST_STRIDE = sizeof(BenchRow) / sizeof(uint32_t);

    // Use one snapshot buffer per core to avoid reporter races and large stack use.
    // Keep its index fixed if the reporter migrates during the read.
    constinit kickos::BenchHistSnap g_dist_snap[KICKOS_KERNEL_CORES] = {};
    static_assert(sizeof(g_dist_snap) / sizeof(g_dist_snap[0]) == KICKOS_KERNEL_CORES,
                  "one percentile snapshot per kernel core, or two reporters share one");

    kickos::BenchHistSnap& dist_snapshot(uint32_t d)
    {
        kickos::BenchHistSnap& s = g_dist_snap[kickos_kernel_core()];
        kickos::bench_hist_snapshot(&g_row[0].dist[d].hist[0], DIST_STRIDE, KICKOS_KERNEL_CORES,
                                    s);
        return s;
    }
#endif

    Acc dist_total(uint32_t d)
    {
        Acc a = {};
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            acc_merge(a, g_row[c].dist[d].acc);
        }
        return a;
    }

    // Keep inline to avoid an extra frame on the switch tail.
    inline __attribute__((always_inline)) void dist_add_row(BenchRow& r, uint32_t d,
                                                            kickos::BenchTick delta)
    {
        DistSlot& sl = r.dist[d];
        if (not acc_add(sl.acc, delta))
        {
            return;
        }
#if !BENCH_HEADLINE_MIN
        sl.hist[kickos::bench_bucket(static_cast<uint32_t>(delta))]++;
#endif
    }

    // Baseline for trap-handler fastpath switches, which bypass BD_SWITCH timing.
    constinit uint32_t g_ipc_fast_base = 0;

    void dist_reset_one(uint32_t d)
    {
        kickos::IrqLock lock;
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            g_row[c].dist[d].acc = Acc{};
#if !BENCH_HEADLINE_MIN
            for (uint32_t i = 0; i < kickos::BENCH_HIST; i++)
            {
                g_row[c].dist[d].hist[i] = 0;
            }
#endif
        }
    }
}

extern "C"
{
    // Switch-entry timestamps must be per core. ARM64 and RV64 keep theirs
    // in their own per-CPU blocks; LX6 indexes this array by core.
    constinit uint32_t g_bench_sw_start[KICKOS_NUM_CORES] = {};
    // Xtensa cannot call from the windowed exit. Record the end in switch.S and
    // add the interval at the next switch entry.
    constinit uint32_t g_bench_sw_end[KICKOS_NUM_CORES] = {};
    // RV32 and RXv3 are single-core here. Save the save/swap and restore timestamps
    // separately because the exit that reloads the frame has no register left to hold a
    // sum. Record the completed interval at the next switch, where a call is possible.
    constinit uint32_t g_bench_sw_half = 0;
    constinit uint32_t g_bench_sw_rstart = 0;
    constinit uint32_t g_bench_sw_pend = 0;

    // C entry point for the deferred MPU commit in the switch epilogue.
    void kickos_bench_mpu_commit(uint32_t delta)
    {
        kickos::bench_phase_add(kickos::PH_MPU_COMMIT, delta);
    }

#if KICKOS_KERNEL_CORES > 1
    // C entry point for the kernel lock's ticket draw, called once per acquisition by the
    // backend that owns the counters.
    void kickos_bench_lock_draw(uint32_t retries, uint32_t queued)
    {
        BenchRow& r = row();
        dist_add_row(r, kickos::BD_LOCK_DRAW, retries);
        dist_add_row(r, kickos::BD_LOCK_QUEUE, queued);
    }

    // One sample per reschedule ask, valued by the peers it reaches. A zero-peer ask is a
    // sample like any other: the omitted request and the empty one are different events.
    void kickos_bench_resched_ask(uint32_t peers)
    {
        dist_add_row(row(), kickos::BD_RESCHED_ASK, peers);
    }

    // One sample per take that STOOD. Its n is what the ask row cannot give: the entries this
    // core actually made because a peer asked, after coalescing.
    void kickos_bench_resched_take(void)
    {
        dist_add_row(row(), kickos::BD_RESCHED_TAKE, 1u);
    }
#endif

    void kickos_bench_switch_done(uint32_t delta)
    {
        dist_add_row(row(), kickos::BD_SWITCH, delta);
    }
}

namespace kickos
{
    constinit BenchLockRow g_bench_lock[KICKOS_KERNEL_CORES] = {};
    constinit Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core = BENCH_CORE_NONE;
    // Atomic because the reporter reads this without acquiring g_e2e_mode, and because the
    // ISR stamp tests it from a core the armer never synchronized with.
    constinit Atomic<int32_t, Order::RELAXED> g_bench_e2e_line = -1;
}

namespace
{
    uint64_t cyccnt_hz()
    {
#if defined(KICKOS_CHIP_CYCCNT_HZ)
        return KICKOS_CHIP_CYCCNT_HZ;
#else
        return arch_cpu_clock_hz();
#endif
    }

    // Both samples and converted nanosecond columns are uint32_t.
    constexpr uint32_t CYC_NS_MAX = 0xFFFFFFFFu;

    // Cap oversized nanosecond conversions and count them separately.
    // Keep the counter print separate to avoid a ninth stack-passed argument.
    uint32_t cyc_to_ns(uint32_t cyc, uint32_t& capped)
    {
        uint64_t const hz = cyccnt_hz();
        if (hz == 0)
        {
            return 0;
        }
        uint64_t const ns = (static_cast<uint64_t>(cyc) * 1000000000ull) / hz;
        if (ns > CYC_NS_MAX)
        {
            capped++;
            return CYC_NS_MAX;
        }
        return static_cast<uint32_t>(ns);
    }

#if KICKOS_KERNEL_CORES > 1
    // Per-core rows use min/avg/max; only the aggregate has percentiles.
    // The lock-hold maximum is the longest measured IrqLock interval on that core.
    constexpr char DIST_ROW_CYC[] = "    core %u: %u/%u/%u cyc  (min/avg/max, n=%u)\n";
    constexpr char DIST_ROW_CNT[] = "    core %u: %u/%u/%u  (min/avg/max, n=%u)\n";

    void dist_print_rows(Acc const* snap, char const* fmt)
    {
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            Acc const& a = snap[c];
            uint32_t min = 0;
            uint32_t avg = 0;
            if (a.count != 0)
            {
                min = a.min;
                avg = static_cast<uint32_t>(a.sum / a.count);
            }
            kickos::kprintf_paced(fmt, static_cast<unsigned>(c), static_cast<unsigned>(min),
                            static_cast<unsigned>(avg), static_cast<unsigned>(a.max),
                            static_cast<unsigned>(a.count));
        }
    }

    void phase_print_rows()
    {
        kickos::kprintf_paced("  per-core phase samples:\n");
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            uint64_t n = 0;
            for (uint32_t i = 0; i < kickos::PH_COUNT; i++)
            {
                n += g_row[c].phase[i].count;
            }
            kickos::kprintf_paced("    core %u  n=%u\n", static_cast<unsigned>(c),
                            static_cast<unsigned>(n));
        }
    }
#else
    void phase_print_rows() {}
#endif

    // Check that conversion of the largest cycle count agrees with the cap counter.
    void ns_probe_print()
    {
        if (cyccnt_hz() == 0)
        {
            kickos::kprintf_paced("  ns-probe: rate=0 (nothing converts, so no ns column is"
                            " printed)\n");
            return;
        }
        uint32_t capped = 0;
        uint32_t const ns = cyc_to_ns(CYC_NS_MAX, capped);
        kickos::kprintf_paced("  ns-probe: cyc=%u ns=%u capped=%u\n",
                        static_cast<unsigned>(CYC_NS_MAX), static_cast<unsigned>(ns),
                        static_cast<unsigned>(capped));
    }

    // Check that one valid and one oversized sample update count and sat separately.
    void sat_probe_print()
    {
#if KICKOS_BENCH_TICK_BITS == 64
        g_sat_probe = Acc{};
        acc_add(g_sat_probe, static_cast<kickos::BenchTick>(SAT_PROBE_FITS));
        uint32_t const fit_n = g_sat_probe.count;
        uint32_t const fit_s = g_sat_probe.sat;
        acc_add(g_sat_probe, static_cast<kickos::BenchTick>(SAT_PROBE_WIDE));
        kickos::kprintf_paced("  sat-probe: width=64 fits+%u/%u wide+%u/%u max=%u\n",
                        static_cast<unsigned>(fit_n), static_cast<unsigned>(fit_s),
                        static_cast<unsigned>(g_sat_probe.count - fit_n),
                        static_cast<unsigned>(g_sat_probe.sat - fit_s),
                        static_cast<unsigned>(g_sat_probe.max));
#else
        kickos::kprintf_paced("  sat-probe: width=32 (no delta this counter forms is too wide)\n");
#endif
    }
}

namespace kickos
{
    void bench_phase_add(uint32_t phase, BenchTick delta)
    {
        if (phase >= PH_COUNT)
        {
            return;
        }
        acc_add(row().phase[phase], delta);
    }

    void bench_dist_add(uint32_t dist, BenchTick delta)
    {
        if (dist >= BD_COUNT)
        {
            return;
        }
        dist_add_row(row(), dist, delta);
    }

    // Keep the release address of the sample that sets the maximum.
    void bench_lock_hold_add(BenchTick delta, void* site)
    {
        BenchRow& r = row();
        Acc const& a = r.dist[BD_LOCK_HOLD].acc;
        uint32_t const had = a.count;
        uint32_t const peak = a.max;
        dist_add_row(r, BD_LOCK_HOLD, delta);
        if (a.count != had and (had == 0 or a.max != peak))
        {
            // Publish only when the maximum changes or the first sample arrives.
            uint32_t const gen = r.lock_site_gen.load(std::memory_order_relaxed);
            r.lock_site_gen.store(gen + 1u, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            r.lock_site.store(reinterpret_cast<uintptr_t>(site), std::memory_order_relaxed);
            r.lock_site_max.store(a.max, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            r.lock_site_gen.store(gen + 2u, std::memory_order_relaxed);
        }
    }

    // Read this core only so probes detect samples written to the wrong row.
    uint32_t bench_dist_count(uint32_t dist)
    {
        if (dist >= BD_COUNT)
        {
            return 0;
        }
        return row().dist[dist].acc.count;
    }

    void bench_reset(uint32_t fast_taken)
    {
        // Mask local updates while clearing rows. Peer cores can still add samples.
        IrqLock lock;
        // Discard delayed Xtensa, RV32 and RXv3 switch samples from the previous window.
        for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
        {
            g_bench_sw_end[c] = 0;
        }
        g_bench_sw_half = 0;
        g_bench_sw_pend = 0;
        g_ipc_fast_base = fast_taken;
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            BenchRow& r = g_row[c];
            for (uint32_t d = 0; d < BD_COUNT; d++)
            {
                r.dist[d].acc = Acc{};
#if !BENCH_HEADLINE_MIN
                for (uint32_t i = 0; i < BENCH_HIST; i++)
                {
                    r.dist[d].hist[i] = 0;
                }
#endif
            }
            for (uint32_t i = 0; i < PH_COUNT; i++)
            {
                r.phase[i] = Acc{};
            }
            r.lock_site.store(0, std::memory_order_relaxed);
            r.lock_site_max.store(0, std::memory_order_relaxed);
            r.lock_site_gen.store(0, std::memory_order_relaxed);
        }
        // Reset counters with their distributions. Preserve the one-time tare and
        // the live waiter state, which may already be armed on another core.
        g_e2e_closed = 0;
        g_e2e_dropped = 0;
        g_e2e_raised = 0;
        // Exclude the reset work from the enclosing lock-hold sample.
        g_bench_lock[kickos_kernel_core()].start = bench_cyccnt();
    }

    // Print aggregate and per-core values. The caller selects the span label.
    static void dist_print_fmt(uint32_t d, DistFmt const& f)
    {
        // Snapshot buckets before the accumulator and compute all output from that copy.
        // Concurrent samples may appear in n/min/max/sum but not the bucket quantiles.
#if !BENCH_HEADLINE_MIN
        kickos::BenchHistSnap const& hist = dist_snapshot(d);
#endif
#if KICKOS_KERNEL_CORES > 1
        Acc snap[KICKOS_KERNEL_CORES];
        Acc agg = {};
        for (uint32_t i = 0; i < KICKOS_KERNEL_CORES; i++)
        {
            snap[i] = g_row[i].dist[d].acc;
            acc_merge(agg, snap[i]);
        }
#else
        Acc const agg = g_row[0].dist[d].acc;
#endif
        uint32_t const c = agg.count;
#if BENCH_HEADLINE_MIN
        uint32_t lo = 0;
        uint32_t mid = 0;
        if (c != 0)
        {
            lo = agg.min;
            mid = static_cast<uint32_t>(agg.sum / c);
        }
#else
        uint32_t const lo = bench_hist_percentile(hist, 1, 2);
        uint32_t mid = bench_hist_percentile(hist, 99, 100);
        if (c < DIST_P99_FLOOR)
        {
            mid = agg.max;
        }
#endif
        // A slot with no nanosecond pair carries a count, which no clock converts.
        bool const counted = f.ns.p99 == nullptr;
        if (cyccnt_hz() == 0 or counted)
        {
            kprintf_paced(stat_fmt(f.cyc, c), static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                    static_cast<unsigned>(agg.max), static_cast<unsigned>(c));
        }
        else
        {
            uint32_t capped = 0;
            uint32_t const ns_lo = cyc_to_ns(lo, capped);
            uint32_t const ns_mid = cyc_to_ns(mid, capped);
            uint32_t const ns_max = cyc_to_ns(agg.max, capped);
            kprintf_paced(stat_fmt(f.ns, c), static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                    static_cast<unsigned>(agg.max), static_cast<unsigned>(ns_lo),
                    static_cast<unsigned>(ns_mid),
                    static_cast<unsigned>(ns_max), static_cast<unsigned>(c));
            if (capped != 0)
            {
                kickos::kprintf_paced("    %u ns column(s) capped: the span outruns the field\n",
                                static_cast<unsigned>(capped));
            }
        }
#if KICKOS_BENCH_TICK_BITS == 64
        if (agg.sat != 0)
        {
            kickos::kprintf_paced("    dropped %u sample(s) wider than the accumulator\n",
                            static_cast<unsigned>(agg.sat));
        }
#endif
#if KICKOS_KERNEL_CORES > 1
        char const* row = DIST_ROW_CYC;
        if (counted)
        {
            row = DIST_ROW_CNT;
        }
        dist_print_rows(snap, row);
#endif
    }

    // Print an aggregate distribution in nanoseconds.
    static void dist_print_ns(uint32_t d, StatFmt const& fmt)
    {
#if !BENCH_HEADLINE_MIN
        kickos::BenchHistSnap const& hist = dist_snapshot(d);
#endif
        Acc const agg = dist_total(d);
        uint32_t const c = agg.count;
#if BENCH_HEADLINE_MIN
        uint32_t lo = 0;
        uint32_t mid = 0;
        if (c != 0)
        {
            lo = agg.min;
            mid = static_cast<uint32_t>(agg.sum / c);
        }
#else
        uint32_t const lo = bench_hist_percentile(hist, 1, 2);
        uint32_t mid = bench_hist_percentile(hist, 99, 100);
        if (c < DIST_P99_FLOOR)
        {
            mid = agg.max;
        }
#endif
        kprintf_paced(stat_fmt(fmt, c), static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                static_cast<unsigned>(agg.max), static_cast<unsigned>(c));
    }

    uint32_t bench_dist_print(uint32_t fast_taken)
    {
        kprintf_paced("  switch-probe: fastpath-swaps=%u  (swapped inside the trap, so not in"
                      " the switch row)\n",
                static_cast<unsigned>(fast_taken - g_ipc_fast_base));
        for (uint32_t d = 0; d < DIST_SWEPT_FIRST; d++)
        {
            dist_print_fmt(d, DIST_FMT[d]);
        }
        // Read each published maximum with its release address. Samples may have
        // arrived since the aggregate was printed. Site 0 means no sample.
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            BenchRow const& lr = g_row[c];
            uintptr_t site = 0;
            uint32_t peak = 0;
            uint32_t g0 = 0;
            uint32_t g1 = 0;
            uint32_t tries = 0;
            do
            {
                g0 = lr.lock_site_gen.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                site = lr.lock_site.load(std::memory_order_relaxed);
                peak = lr.lock_site_max.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                g1 = lr.lock_site_gen.load(std::memory_order_relaxed);
                tries++;
            } while ((g0 != g1 or (g0 & 1u) != 0u) and tries < LOCK_SITE_READ_TRIES);

            if ((g0 != g1 or (g0 & 1u) != 0u))
            {
                // Mark the pair as unusable if publication overlaps every read attempt.
                kprintf_paced("  lock-site: core=%u site=%p max=%u unstable=1\n",
                        static_cast<unsigned>(c), reinterpret_cast<void*>(site),
                        static_cast<unsigned>(peak));
            }
            else
            {
                kprintf_paced("  lock-site: core=%u site=%p max=%u\n", static_cast<unsigned>(c),
                        reinterpret_cast<void*>(site), static_cast<unsigned>(peak));
            }
        }
        return dist_total(BD_SWITCH).count;
    }

    void bench_lock_probe_print()
    {
        uint32_t const hold0 = bench_dist_count(BD_LOCK_HOLD);
#if KICKOS_KERNEL_CORES > 1
        uint32_t const wait0 = bench_dist_count(BD_LOCK_WAIT);
#endif
        uint32_t const core = kickos_kernel_core();
        // The probe must start outside an existing lock bracket.
        uint32_t const depth0 = g_bench_lock[core].depth;
        {
            IrqLock outer;
            {
                IrqLock middle;
                {
                    IrqLock inner;
                }
            }
        }
#if KICKOS_KERNEL_CORES > 1
        kprintf_paced("  lock-probe: hold+%u wait+%u core=%u depth=%u"
                "  (three nested, one outermost)\n",
                static_cast<unsigned>(bench_dist_count(BD_LOCK_HOLD) - hold0),
                static_cast<unsigned>(bench_dist_count(BD_LOCK_WAIT) - wait0),
                static_cast<unsigned>(core), static_cast<unsigned>(depth0));
#else
        kprintf_paced("  lock-probe: hold+%u core=%u depth=%u  (three nested, one outermost)\n",
                static_cast<unsigned>(bench_dist_count(BD_LOCK_HOLD) - hold0),
                static_cast<unsigned>(core), static_cast<unsigned>(depth0));
#endif
    }

#if KICKOS_KERNEL_CORES > 1
    // Measure one complete round: request peers and wait for all replies.
    // Both timestamps come from this core because counters are not synchronized.
    // Placement precedes the IrqLock; peers reply without scheduling, so no
    // switch occurs inside the interval. All rounds share one lock-hold sample.
    uint32_t bench_doorbell_probe_print(uint32_t core, uint32_t rounds)
    {
        Thread* const self = sched::current();
        uint32_t const saved = self->affinity;
        // Narrowing the current affinity cannot exceed the task grant.
        uint32_t const want = saved & (1u << core);
        if (want == 0)
        {
            return 0;
        }
        sched::set_affinity(self, want);

        uint32_t me = 0;
        uint32_t moved = 0;
        uint32_t ran = 0;
        uint32_t raise_lo = 0xFFFFFFFFu;
        {
            IrqLock lock;
            me = kickos_kernel_core();
            uint32_t peers = 0;
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                if (c != me)
                {
                    peers |= 1u << c;
                }
            }
            uint32_t const before = bench_dist_count(BD_DOORBELL);
            while (ran < rounds)
            {
                KICKOS_BENCH_MARK(bd);
                arch_ipi_send(peers);
                // Measure the raise alone as a control within the same round.
                uint32_t const mid = bench_cyccnt();
                arch_ipi_wait(peers);
                KICKOS_BENCH_DIST_SPAN(BD_DOORBELL, bd);
                uint32_t const raised = mid - bd;
                if (raised < raise_lo)
                {
                    raise_lo = raised;
                }
                ran++;
            }
            // Check this core under the same mask to detect samples written to another row.
            moved = bench_dist_count(BD_DOORBELL) - before;
        }
        sched::set_affinity(self, saved);

        if (ran == 0)
        {
            raise_lo = 0;
        }
        kprintf_paced("  doorbell-probe: asked=%u on=%u ran=%u db+%u raise=%u\n",
                static_cast<unsigned>(core), static_cast<unsigned>(me),
                static_cast<unsigned>(ran), static_cast<unsigned>(moved),
                static_cast<unsigned>(raise_lo));
        return ran;
    }
#endif

    void bench_phase_print()
    {
        // Print the expected row count so truncated output can be detected.
        kprintf_paced("  phase table (%u rows; cyc avg/max, min last and a floor; leaf -= NULL,"
                      " composite -= NULL + k*(NEST-NULL)):\n",
                      static_cast<unsigned>(PH_COUNT));
        for (uint32_t i = 0; i < PH_COUNT; i++)
        {
            Acc a = {};
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                acc_merge(a, g_row[c].phase[i]);
            }
            uint32_t min = 0;
            uint32_t avg = 0;
            if (a.count != 0)
            {
                min = a.min;
                avg = static_cast<uint32_t>(a.sum / a.count);
            }
#if KICKOS_BENCH_TICK_BITS == 64
            // Statistics exclude SAT, the count of unrepresentable spans.
            if (a.sat != 0)
            {
                kprintf_paced("    %s %u/%u  min=%u  n=%u  SAT=%u\n", PHASE_NAME[i],
                        static_cast<unsigned>(avg), static_cast<unsigned>(a.max),
                        static_cast<unsigned>(min), static_cast<unsigned>(a.count),
                        static_cast<unsigned>(a.sat));
                continue;
            }
#endif
            kprintf_paced("    %s %u/%u  min=%u  n=%u\n", PHASE_NAME[i], static_cast<unsigned>(avg),
                    static_cast<unsigned>(a.max), static_cast<unsigned>(min),
                    static_cast<unsigned>(a.count));
        }
        phase_print_rows();
        ns_probe_print();
        sat_probe_print();
    }

    uint64_t bench_cyccnt_hz() { return cyccnt_hz(); }

    // Attach the bench handler to a spare line and unmask it. Call once.
    int bench_irq_setup(int line)
    {
        if (not irq_attach(line, bench_irq_handler, nullptr))
        {
            return -KOS_EBUSY;
        }
        g_irq_line = line;
        irq_line_op(line, LineOp::CLEAR); // Discard pending events before arming.
        irq_line_op(line, LineOp::UNMASK);
        return 0;
    }

    // Count missing delivery and foreign-core timestamps separately; neither
    // contributes to the distribution.
    enum IrqVerdict : uint32_t
    {
        IRQ_SILENT = 0,
        IRQ_FOREIGN,
        IRQ_LOCAL
    };

    struct IrqSample
    {
        uint32_t cyc;     // 0 unless the verdict is IRQ_LOCAL
        uint32_t window;  // raise to observation, on the raising core
        uint32_t core;    // Handler core for both classification and accounting.
        uint32_t verdict;
    };

    // Check both the handler's core ID and whether its timestamp falls between
    // two local reads. Reject wrapped intervals. These checks detect mismatched
    // clock domains independently. Disable the interval check on glitching
    // counters, where valid reads can be inflated and only minima are meaningful.
#if defined(KICKOS_CHIP_CYCCNT_GLITCHES) && KICKOS_CHIP_CYCCNT_GLITCHES
#define BENCH_IRQ_WINDOW_ARM 0
#else
#define BENCH_IRQ_WINDOW_ARM 1
#endif
    static IrqSample irq_close(uint32_t t0)
    {
        for (uint32_t i = 0; i < 100000u and g_irq_seen == 0; i++)
        {
            __asm volatile("nop");
        }
        IrqSample s = {0, 0, BENCH_CORE_NONE, IRQ_SILENT};
        if (g_irq_seen == 0)
        {
            return s; // No interrupt observed.
        }
        s.window = bench_cyccnt() - t0;
        s.core = g_irq_core;
        uint32_t const d = g_irq_entry - t0;
        bool outside = false;
#if BENCH_IRQ_WINDOW_ARM
        outside = d > s.window;
#endif
        if (outside or s.core != kickos_kernel_core())
        {
            s.verdict = IRQ_FOREIGN;
            return s;
        }
        // Reserve zero for no delivery. Round a valid sub-tick latency up to one.
        s.cyc = d;
        if (s.cyc == 0)
        {
            s.cyc = 1;
        }
        s.verdict = IRQ_LOCAL;
        return s;
    }

    // One IRQ-entry-latency sample in cycles (0 if the line did not fire, or the arch
    // has no cycle counter / no injectable line).
    static IrqSample irq_once(int line)
    {
        // Rearm before each injection; some backends mask the line on delivery.
        irq_line_op(line, LineOp::UNMASK);
        g_irq_seen = 0;
        uint32_t const t0 = bench_cyccnt();
        bench_irq_raise(line);
        return irq_close(t0);
    }

    struct IrqPlacement
    {
        uint32_t saved;
        uint32_t on;
    };

    // Try allowed cores until the handler runs on the initiating core.
    // If none matches, retain the sweep and report rejected samples.
    // Narrowing the existing affinity mask needs no additional permission.
    static IrqPlacement irq_place()
    {
        IrqPlacement p = {0, 0};
#if KICKOS_KERNEL_CORES > 1
        Thread* const self = sched::current();
        p.saved = self->affinity;
        for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
        {
            uint32_t const want = p.saved & (1u << c);
            if (want == 0)
            {
                continue;
            }
            sched::set_affinity(self, want);
            if (irq_once(g_irq_line).verdict == IRQ_LOCAL)
            {
                break;
            }
        }
#endif
        p.on = kickos_kernel_core();
        return p;
    }

    static void irq_unplace(IrqPlacement const& p)
    {
#if KICKOS_KERNEL_CORES > 1
        sched::set_affinity(sched::current(), p.saved);
#else
        (void)p;
#endif
    }

    // Track the first handler core and flag any later change, even for rejected samples.
    struct IrqTally
    {
        uint32_t raised;
        uint32_t foreign;
        uint32_t window; // Maximum observed interval on the raising core.
        uint32_t hcore;
        uint32_t mixed;
    };

    static void irq_tally(IrqTally& t, IrqSample const& s)
    {
        t.raised++;
        if (s.window > t.window)
        {
            t.window = s.window;
        }
        if (s.verdict == IRQ_SILENT)
        {
            return;
        }
        if (t.hcore == BENCH_CORE_NONE)
        {
            t.hcore = s.core;
        }
        else if (t.hcore != s.core)
        {
            t.mixed = 1;
        }
        if (s.verdict == IRQ_FOREIGN)
        {
            t.foreign++;
        }
    }

    // Report requested, raised and foreign-core counts to distinguish missing
    // delivery from rejected samples. win bounds accepted same-core samples.
    static void irq_tally_print(char const* fmt, char const* fmt_none, char const* fmt_mixed,
            IrqTally const& t, IrqPlacement const& p)
    {
        if (t.hcore == BENCH_CORE_NONE or t.mixed != 0)
        {
            char const* f = fmt_none;
            if (t.mixed != 0)
            {
                f = fmt_mixed;
            }
            kprintf_paced(f, static_cast<unsigned>(g_irq_line), static_cast<unsigned>(p.on),
                    static_cast<unsigned>(t.raised), static_cast<unsigned>(t.foreign),
                    static_cast<unsigned>(t.window));
            return;
        }
        kprintf_paced(fmt, static_cast<unsigned>(g_irq_line), static_cast<unsigned>(p.on),
                static_cast<unsigned>(t.raised), static_cast<unsigned>(t.hcore),
                static_cast<unsigned>(t.foreign), static_cast<unsigned>(t.window));
    }

    // Measure inject-to-handler latency across an interrupt-masked copy.
    // The interval includes the masked work and exception entry.
    // Return zero if injection is unsupported.
    static IrqSample irq_masked_once(int line, uint32_t span_bytes)
    {
        if (span_bytes > BENCH_LAT_SPAN_MAX)
        {
            span_bytes = BENCH_LAT_SPAN_MAX;
        }
        irq_line_op(line, LineOp::UNMASK);
        g_irq_seen = 0;
        arch_irq_state_t st = arch_irq_save();
        uint32_t const t0 = bench_cyccnt();
        bench_irq_raise(line);
        for (uint32_t i = 0; i < span_bytes; i++)
        {
            g_lat_dst[i] = g_lat_src[i];
        }
        arch_irq_restore(st);
        // Only this core is masked. Reject an interrupt delivered to a peer during the copy.
        return irq_close(t0);
    }

    // Reset each sweep slot and always report its count, including zero.
    uint32_t bench_irq_sweep(uint32_t samples)
    {
        dist_reset_one(BD_IRQ_ENTRY);
        IrqPlacement const p = irq_place();
        IrqTally t = {0, 0, 0, BENCH_CORE_NONE, 0};
        for (uint32_t i = 0; i < samples and g_irq_line >= 0; i++)
        {
            IrqSample const s = irq_once(g_irq_line);
            irq_tally(t, s);
            if (s.verdict == IRQ_LOCAL)
            {
                bench_dist_add(BD_IRQ_ENTRY, s.cyc);
            }
        }
        irq_unplace(p);
        irq_tally_print(BENCH_IRQ_PROBE_FMT("irq-probe"),
                BENCH_IRQ_PROBE_FMT_NONE("irq-probe"), BENCH_IRQ_PROBE_FMT_MIXED("irq-probe"),
                t, p);
        dist_print_fmt(BD_IRQ_ENTRY, DIST_FMT[BD_IRQ_ENTRY]);
        return dist_total(BD_IRQ_ENTRY).count;
    }

    uint32_t bench_irq_wcase_spans()
    {
        return sizeof(WCASE_SPANS) / sizeof(WCASE_SPANS[0]);
    }

    uint32_t bench_irq_wcase_sweep(uint32_t span_index, uint32_t samples)
    {
        if (span_index >= bench_irq_wcase_spans())
        {
            return 0;
        }
        dist_reset_one(BD_IRQ_WCASE);
        IrqPlacement const p = irq_place();
        IrqTally t = {0, 0, 0, BENCH_CORE_NONE, 0};
        for (uint32_t i = 0; i < samples and g_irq_line >= 0; i++)
        {
            IrqSample const s = irq_masked_once(g_irq_line, WCASE_SPANS[span_index]);
            irq_tally(t, s);
            if (s.verdict == IRQ_LOCAL)
            {
                bench_dist_add(BD_IRQ_WCASE, s.cyc);
            }
        }
        irq_unplace(p);
        irq_tally_print(BENCH_IRQ_PROBE_FMT("wcase-probe"),
                BENCH_IRQ_PROBE_FMT_NONE("wcase-probe"),
                BENCH_IRQ_PROBE_FMT_MIXED("wcase-probe"), t, p);
        dist_print_fmt(BD_IRQ_WCASE, WCASE_FMT[span_index]);
        return dist_total(BD_IRQ_WCASE).count;
    }

    // Record the waiter, its IRQ and its switch count for validation at close.
    int bench_e2e_arm(int line)
    {
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -KOS_EINVAL;
        }
        IrqLock lock;
        g_bench_e2e_line = line;
        g_e2e_waiter = sched::current();
        g_e2e_epoch = g_e2e_waiter->switch_count;
        // Clear the ISR result before publishing ARMED to the raiser.
        g_bench_e2e_isr_core = BENCH_CORE_NONE;
        g_e2e_mode = E2E_ARMED;
        return 0;
    }

    // Publish PARKED under the ISR wake lock so delivery cannot precede the park.
    void bench_e2e_park_mark()
    {
        if (g_e2e_mode != E2E_ARMED or sched::current() != g_e2e_waiter)
        {
            return;
        }
        g_e2e_mode = E2E_PARKED;
    }

    // Do not hold IrqLock: this measures unmasked delivery.
    int bench_e2e_raise()
    {
        // Acquire the metadata published by the waiter.
        if (g_e2e_mode != E2E_PARKED)
        {
            return -KOS_EBUSY;
        }
        int const line = g_bench_e2e_line;
        if (g_e2e_waiter == nullptr or line < 0)
        {
            return -KOS_EINVAL;
        }
        // Publish the timestamp immediately through the following release store.
        g_e2e_t0 = arch_clock_now();
        g_e2e_mode = E2E_RAISED;
        g_e2e_raised = g_e2e_raised + 1;
        bench_irq_raise(line);
        return 0;
    }

    int bench_e2e_tare()
    {
        if (g_e2e_mode != E2E_ARMED or sched::current() != g_e2e_waiter)
        {
            return -KOS_EPERM;
        }
        // Use the same timestamp publication order as a real raise.
        g_e2e_t0 = arch_clock_now();
        g_e2e_mode = E2E_TARED;
        return 0;
    }

    // Read the closing timestamp before validation to exclude validation overhead.
    int bench_e2e_close()
    {
        uint64_t const end = arch_clock_now();
        // Acquire the timestamp published by the raiser.
        uint32_t const mode = g_e2e_mode;
        g_e2e_mode = E2E_IDLE;
        if (mode == E2E_IDLE)
        {
            return -KOS_EINVAL;
        }
        if (sched::current() != g_e2e_waiter)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EPERM;
        }
        // Count wakes without a raise as dropped samples; they have no start timestamp.
        if (mode == E2E_ARMED or mode == E2E_PARKED)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EBUSY;
        }
        uint64_t const dns = end - g_e2e_t0;
        // Reject spans over UINT32_MAX ns. Capping would insert stalls into the distribution.
        if (dns > 0xFFFFFFFFull)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EBUSY;
        }
        uint32_t d = 1; // Round a valid sub-tick span up to one.
        if (dns != 0)
        {
            d = static_cast<uint32_t>(dns);
        }
        if (mode == E2E_TARED)
        {
            acc_add(g_e2e_tare, d);
            return 0;
        }
        uint32_t const isr = g_bench_e2e_isr_core;
        if (isr == BENCH_CORE_NONE or g_e2e_waiter->switch_count == g_e2e_epoch)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EBUSY;
        }
        g_e2e_closed = g_e2e_closed + 1;
#if KICKOS_KERNEL_CORES > 1
        if (isr != kickos_kernel_core())
        {
            bench_dist_add(BD_IRQ_E2E_CROSS, d);
            return 0;
        }
#endif
        bench_dist_add(BD_IRQ_E2E_LOCAL, d);
        return 0;
    }

    void bench_e2e_print(uint32_t asked)
    {
        uint32_t tmin = 0;
        uint32_t tavg = 0;
        if (g_e2e_tare.count != 0)
        {
            tmin = g_e2e_tare.min;
            tavg = static_cast<uint32_t>(g_e2e_tare.sum / g_e2e_tare.count);
        }
        // Keep the method in the literal to avoid a stack-passed printf argument.
        kprintf_paced("  e2e-probe: line=%u closed=%u dropped=%u"
                " tare=%u/%u ns  (min/avg, n=%u)\n",
                static_cast<unsigned>(g_bench_e2e_line), static_cast<unsigned>(g_e2e_closed),
                static_cast<unsigned>(g_e2e_dropped), static_cast<unsigned>(tmin),
                static_cast<unsigned>(tavg), static_cast<unsigned>(g_e2e_tare.count));
        // Compare the requested sweep size with the accepted raise count.
        kprintf_paced("  e2e-passes: asked=%u raised=%u\n", static_cast<unsigned>(asked),
                static_cast<unsigned>(g_e2e_raised));
        dist_print_ns(BD_IRQ_E2E_LOCAL, E2E_FMT_LOCAL);
#if KICKOS_KERNEL_CORES > 1
        dist_print_ns(BD_IRQ_E2E_CROSS, E2E_FMT_CROSS);
#endif
    }
}
