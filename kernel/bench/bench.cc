// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-core benchmark accumulators and histograms (KICKOS_BENCH only).
// Each core writes its own row; reporting sums counts and histogram buckets
// and takes global minima and maxima. Percentiles use bucket lower bounds.
//
// Switch measurements exclude hardware stacking and deferred MPU commits.
// ARMv7-M and RV32 include software save, swap, and restore. RXv3, ARMv8-A,
// RV64, and x86-64 stop at the swap; LX6 stops before the windowed restore.
// ARMv8-A, RV64, and x86-64 measure thread-context switches only. Compare
// results only when their measurement boundaries match.
//
// Chips with KICKOS_CHIP_CYCCNT_GLITCHES report minima without histograms:
// their counter errors can only increase measured intervals. Counter rate
// comes from KICKOS_CHIP_CYCCNT_HZ or the core clock; zero omits time conversion.

#include <kickos/irq_route.h>
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

// The statistic names go in the LITERAL and never in a %s argument, so the label is pasted in
// at compile time too: a ninth kprintf_paced argument spills to the outgoing stack area, and that
// frame is on the SYSPRIV chain the stack-descent gate measures against a red zone.
#if defined(KICKOS_CHIP_CYCCNT_GLITCHES) && KICKOS_CHIP_CYCCNT_GLITCHES
#define BENCH_HEADLINE_MIN 1
#define BENCH_DIST_FMT(label) "  " label " %u/%u/%u cyc  %u/%u/%u ns  (min/avg/max, n=%u)\n"
#define BENCH_DIST_FMT_CYC(label) "  " label " %u/%u/%u cyc  (min/avg/max, n=%u)\n"
#define BENCH_E2E_FMT(label) "  " label " %u/%u/%u ns  (min/avg/max, n=%u)\n"
#else
#define BENCH_HEADLINE_MIN 0
#define BENCH_DIST_FMT(label) "  " label " %u/%u/%u cyc  %u/%u/%u ns  (p50/p99/max, n=%u)\n"
#define BENCH_DIST_FMT_CYC(label) "  " label " %u/%u/%u cyc  (p50/p99/max, n=%u)\n"
#define BENCH_E2E_FMT(label) "  " label " %u/%u/%u ns  (p50/p99/max, n=%u)\n"
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;
    using kickos::bench_cyccnt;

    // IRQ-entry latency: the handler stamps its own entry and its own core here. The seen
    // flag's RELEASE is what publishes both; a relaxed flag orders neither, and a raiser
    // that observed it could read a stamp from the sample before.
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
    // Read by the report from a third thread, which acquires no state: relaxed, so that read
    // is not a race. It is the state that orders it for the raiser.
    constinit Atomic<int32_t, Order::RELAXED> g_e2e_line = -1;
    constinit kickos::Thread* g_e2e_waiter = nullptr;
    constinit uint32_t g_e2e_epoch = 0;
    // Too wide for the house atomic on any target, so it is plain and the state publishes it.
    constinit uint64_t g_e2e_t0 = 0;
    // Same as the line: written by the close alone, read by the report on another thread.
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_closed = 0;
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_dropped = 0;
    // Count requested passes, including those that never parked and closed no sample.
    constinit Atomic<uint32_t, Order::RELAXED> g_e2e_raised = 0;

    // Masked-span body: a byte copy across these models the endpoint copy under IrqLock
    // (bounded by KOS_EP_MSG_MAX). volatile so it is neither elided nor hoisted out of the
    // masked window.
    constexpr uint32_t BENCH_LAT_SPAN_MAX = 1024;
    constinit volatile uint8_t g_lat_src[BENCH_LAT_SPAN_MAX] = {0};
    constinit volatile uint8_t g_lat_dst[BENCH_LAT_SPAN_MAX] = {0};

    // Set the line pending. On ARM a direct STIR write, which works while PRIMASK holds the
    // span masked; elsewhere the arch inject seam, a no-op where no line is software
    // injectable and the sample then reads 0. The report names which one this image took.
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

    // The line bench_irq_setup attached, so the sweeps below cannot be pointed at a second
    // one. Negative until then, and a sweep before the setup injects into nothing.
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
    // irq_masked_once clamps span_bytes to BENCH_LAT_SPAN_MAX, so an entry above it would
    // measure a narrower body than its label names.
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

    // False when the sample was too wide to record. always_inline is load-bearing, not a hint:
    // at -Os an out-of-line copy gives the switch tail a frame, and that tail is a root the
    // stack-descent gate measures.
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

    // The instrument's own tail: the return to userspace, the device read and the trap back
    // in, with no interrupt in it. Every end-to-end figure sits above this. One accumulator
    // and not one per core: a single waiter thread feeds it.
    constinit Acc g_e2e_tare = {};

#if KICKOS_BENCH_TICK_BITS == 64
    // Test saturation with one valid and one oversized delta in a separate
    // accumulator. Keep it static to avoid increasing the measured stack depth.
    constinit Acc g_sat_probe = {};
    constexpr uint32_t SAT_PROBE_FITS = 1000u;
    // Low half 0x2000, which is ABOVE the representable sample: a build that counted this one
    // into the statistics as well moves the maximum off 1000 and says so.
    constexpr uint64_t SAT_PROBE_WIDE = 0x100002000ull;
#endif

    // The count is folded LAST: the minimum arm reads it to tell an empty accumulator from
    // one whose minimum is 0. The saturation count folds ahead of the early return, a row
    // that recorded nothing but saturated samples having a finding to report.
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
        "REPLY_WAKE      ", "REPLY_RECV_TOTAL", "REPLY_RECV_TAIL ",
        "WAKE_UNPARK     ", "PICK_NEXT       ", "SWITCH_TO       ",
        "SWITCH_BOOK     ", "MPU_APPLY       ", "MPU_COMMIT      ", "REENT_SEAT      ",
        "KTIME_REARM     ",
        "ARCH_SWITCH     "};
    static_assert(sizeof(PHASE_NAME) / sizeof(PHASE_NAME[0]) == kickos::PH_COUNT,
                  "one name per phase, in enum order");

    // --- One row per kernel core ----------------------------------------------
    // One label per distribution, in BD_ order, pasted into the format literal.
    struct DistFmt
    {
        char const* ns;
        char const* cyc;
    };
    constexpr DistFmt DIST_FMT[kickos::BD_COUNT] = {
        {BENCH_DIST_FMT("switch:   "), BENCH_DIST_FMT_CYC("switch:   ")},
        {BENCH_DIST_FMT("lock-hold:"), BENCH_DIST_FMT_CYC("lock-hold:")},
#if KICKOS_KERNEL_CORES > 1
        {BENCH_DIST_FMT("lock-wait:"), BENCH_DIST_FMT_CYC("lock-wait:")},
        {BENCH_DIST_FMT("doorbell: "), BENCH_DIST_FMT_CYC("doorbell: ")},
#endif
        {BENCH_DIST_FMT("irq:      "), BENCH_DIST_FMT_CYC("irq:      ")},
        // The worst-case slot's label names the span, which is a run-time choice, so its row
        // comes from WCASE_FMT below. A %s here would be a NINTH kprintf_paced argument and would
        // spill onto the stack-descent chain the red-zone gate measures.
        {nullptr, nullptr},
        // The end-to-end rows are NANOSECONDS and not cycles, and the reason is the machine:
        // a span that opens on one core and closes on another cannot be timed with a per-core
        // cycle counter, the two counters having no common zero. Their labels are in E2E_FMT.
        {nullptr, nullptr},
#if KICKOS_KERNEL_CORES > 1
        {nullptr, nullptr},
#endif
    };
    static_assert(sizeof(DIST_FMT) / sizeof(DIST_FMT[0]) == kickos::BD_COUNT,
                  "one label per distribution, in enum order");

    // One slot, four labels.
    constexpr DistFmt WCASE_FMT[] = {
        {BENCH_DIST_FMT("wcase-irq[0B]:   "), BENCH_DIST_FMT_CYC("wcase-irq[0B]:   ")},
        {BENCH_DIST_FMT("wcase-irq[64B]:  "), BENCH_DIST_FMT_CYC("wcase-irq[64B]:  ")},
        {BENCH_DIST_FMT("wcase-irq[256B]: "), BENCH_DIST_FMT_CYC("wcase-irq[256B]: ")},
        {BENCH_DIST_FMT("wcase-irq[1024B]:"), BENCH_DIST_FMT_CYC("wcase-irq[1024B]:")},
    };
    static_assert(sizeof(WCASE_FMT) / sizeof(WCASE_FMT[0])
                      == sizeof(WCASE_SPANS) / sizeof(WCASE_SPANS[0]),
                  "one label per masked span, in span order");

    constexpr char const* E2E_FMT_LOCAL = BENCH_E2E_FMT("e2e-local:");
#if KICKOS_KERNEL_CORES > 1
    constexpr char const* E2E_FMT_CROSS = BENCH_E2E_FMT("e2e-cross:");
#endif

    // Where the workload-fed slots end and the swept ones begin. bench_dist_print walks the
    // first group only: a swept slot printed with them reports the window before its sweep.
    constexpr uint32_t DIST_SWEPT_FIRST = kickos::BD_IRQ_ENTRY;

    // The buckets are uint32_t and stay so: they are the narrow term in the bounded-run
    // invariant bench_hist.h states. The bench app's rep counts carry the static_assert.
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
    };

#if KICKOS_KERNEL_CORES > 1
    static_assert(sizeof(BenchRow) % KICKOS_BENCH_CACHE_LINE == 0,
                  "a row shorter than a line would share one with the next writer");
#endif

    constinit BenchRow g_row[KICKOS_KERNEL_CORES] = {};

    BenchRow& row() { return g_row[kickos_kernel_core()]; }

#if !BENCH_HEADLINE_MIN
    static_assert(sizeof(BenchRow) % sizeof(uint32_t) == 0,
                  "the stride between two rows must be a whole number of buckets");
    constexpr uint32_t DIST_STRIDE = sizeof(BenchRow) / sizeof(uint32_t);

    // Use one snapshot buffer per core to avoid reporter races and large stack use.
    // Keep its index fixed if the reporter migrates during the read.
    constinit kickos::BenchHistSnap g_dist_snap[KICKOS_KERNEL_CORES] = {};
    // A buffer narrowed to one entry still compiles against the index below and reads out of
    // bounds at every core but zero.
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

    // always_inline for the same reason acc_add is: the switch tail reaches this and that tail
    // is a root the stack-descent gate measures.
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

    // Reset this slot on every core.
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
    // Xtensa only: the windowed exit can't host a call, so switch.S stamps the switch
    // END here and accumulates (end-start) at the NEXT switch entry (a safe call site).
    constinit uint32_t g_bench_sw_end[KICKOS_NUM_CORES] = {};
    // RV32 is single-core. Save the save/swap and restore timestamps separately
    // because the trap exit has no free register. Record the completed interval
    // at the next switch, where a call is possible.
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
        dist_add_row(row(), kickos::BD_SWITCH, delta);
    }
}

namespace kickos
{
    constinit BenchLockRow g_bench_lock[KICKOS_KERNEL_CORES] = {};
    constinit Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core = BENCH_CORE_NONE;
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

    // The widest sample a 32-bit accumulator holds, which is also the widest value the ns
    // columns can carry: both are uint32_t.
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
    // min/avg/max and not a percentile: the percentile walk above is over the SUMMED
    // histogram, which is the object the headline reports. On the lock-hold row the MAX column
    // is the longest interrupt-masked window that core took under an IrqLock.
    void dist_print_rows(Acc const* snap)
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
            kickos::kprintf_paced("    core %u: %u/%u/%u cyc  (min/avg/max, n=%u)\n",
                            static_cast<unsigned>(c), static_cast<unsigned>(min),
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

    // One representable delta and one that is not, each answered by what it did to the count
    // and to the saturation counter. A row printing no SAT is otherwise indistinguishable
    // from a build whose saturation arm never fires and drops the samples in silence.
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

    // THIS core's row and never the aggregate: a probe reading the sum could not tell an
    // accumulator that wrote a peer's row from one that wrote its own.
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
        // Drop any un-banked sample, banked one switch late on the xtensa and the rv32: else
        // the previous window's last switch leaks into this window's min/max.
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
        }
        // Reset counters with their distributions. Preserve the one-time tare and
        // the live waiter state, which may already be armed on another core.
        g_e2e_closed = 0;
        g_e2e_dropped = 0;
        g_e2e_raised = 0;
        // The clear above is itself a long masked window, and the bracket holding it is still
        // open: without this re-stamp its sample would be this instrument's own cost and would
        // own the lock-hold maximum on every board.
        g_bench_lock[kickos_kernel_core()].start = bench_cyccnt();
    }

    // One distribution, aggregated over the cores and then row by row, under a label the
    // caller chooses: the worst-case slot is re-reported once per span size.
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
        uint32_t const mid = bench_hist_percentile(hist, 99, 100);
#endif
        if (cyccnt_hz() == 0)
        {
            kprintf_paced(f.cyc, static_cast<unsigned>(lo), static_cast<unsigned>(mid),
                    static_cast<unsigned>(agg.max), static_cast<unsigned>(c));
        }
        else
        {
            uint32_t capped = 0;
            uint32_t const ns_lo = cyc_to_ns(lo, capped);
            uint32_t const ns_mid = cyc_to_ns(mid, capped);
            uint32_t const ns_max = cyc_to_ns(agg.max, capped);
            kprintf_paced(f.ns, static_cast<unsigned>(lo), static_cast<unsigned>(mid),
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
        dist_print_rows(snap);
#endif
    }

    // One distribution in NANOSECONDS, aggregated over the cores.
    static void dist_print_ns(uint32_t d, char const* fmt)
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
        uint32_t const mid = bench_hist_percentile(hist, 99, 100);
#endif
        kprintf_paced(fmt, static_cast<unsigned>(lo), static_cast<unsigned>(mid),
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
        return dist_total(BD_SWITCH).count;
    }

    void bench_lock_probe_print()
    {
        uint32_t const hold0 = bench_dist_count(BD_LOCK_HOLD);
#if KICKOS_KERNEL_CORES > 1
        uint32_t const wait0 = bench_dist_count(BD_LOCK_WAIT);
#endif
        uint32_t const core = kickos_kernel_core();
        // Reported because it is the probe's own precondition: hold+1 says nothing about the
        // outermost bracket unless the probe itself started outside one.
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
        // A SUBSET OF WHAT THIS THREAD ALREADY HOLDS, so the affinity invariant needs no grant
        // re-check: narrowing can never leave the task's core set.
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
            // Read under the same mask as the rounds: THIS core's row alone, so a sample
            // written to a peer's row reads back as a count that never moved.
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
            // SAT is the count of spans too wide to state, and every column left of it is
            // over n and not over n + SAT.
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
        irq_line_op(line, LineOp::CLEAR); // discard pre-arm garbage (latch-and-coalesce contract)
        irq_line_op(line, LineOp::UNMASK);
        return 0;
    }

    // A sample and what it is worth. SILENT and FOREIGN feed no statistic and are counted
    // apart: a line that did not fire and one whose handler stamped in another core's clock
    // are different findings, and the row alone shows neither.
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
        uint32_t core;    // the core the handler stamped on, carried so the tally and the
                          // verdict cannot read two different interrupts
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
            return s; // genuinely did not fire (no injectable line / masked)
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
        // 0 is the sentinel for "did not fire", so a real but sub-counter-tick latency
        // (delta==0, e.g. RX's coarse 133 ns CMTW1 tick) must report as 1 rather than be
        // discarded by the caller's `!= 0` fired-check.
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

    // Where a sweep runs, and the mask to hand back afterwards.
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

    // What a sweep did beside the row it filled. `hcore` holds the first core that stamped and
    // `mixed` records that a later one differed, which no per-sample verdict can hide: a row
    // fed from two counters is cross-domain whatever each sample claimed about itself.
    struct IrqTally
    {
        uint32_t raised;
        uint32_t foreign;
        uint32_t window; // widest one the raising core measured; every accepted sample is under it
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
        // A mask holds off THIS core alone. A line delivered to a peer fires during the span
        // instead of at the restore, and irq_close refuses what that peer stamped.
        return irq_close(t0);
    }

    // Both sweeps fill their slot FROM SCRATCH and print one row, so a count is this sweep's
    // and never a running total. A count of 0 is printed too: a line the controller refuses to
    // raise and a fast one produce the same row without it.
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

    // The waiter names itself and the line it holds, then parks. The switch count taken here
    // is what the close compares against.
    int bench_e2e_arm(int line)
    {
        if (line < 0 or line >= KICKOS_MAX_IRQ)
        {
            return -KOS_EINVAL;
        }
        IrqLock lock;
        g_e2e_line = line;
        g_e2e_waiter = sched::current();
        g_e2e_epoch = g_e2e_waiter->switch_count;
        // Clear the ISR result before publishing ARMED to the raiser.
        g_bench_e2e_isr_core = BENCH_CORE_NONE;
        g_e2e_mode = E2E_ARMED;
        return 0;
    }

    // Called by the waiter on its own way into the block, under the kernel lock the ISR's
    // post has to take to reach it: a line injected once this is visible cannot be delivered
    // before the park it announces has committed.
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
        // The acquire, and the only reason the four cells the arm wrote may be read below.
        if (g_e2e_mode != E2E_PARKED)
        {
            return -KOS_EBUSY;
        }
        int const line = g_e2e_line;
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
        // Stamp then state, as the raise does: the floor has to price the same publication the
        // figures above it carry.
        g_e2e_t0 = arch_clock_now();
        g_e2e_mode = E2E_TARED;
        return 0;
    }

    // The closing stamp is the FIRST statement: a test ahead of it would be inside every
    // sample.
    int bench_e2e_close()
    {
        uint64_t const end = arch_clock_now();
        // The acquire, and the only reason t0 may be read below.
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
        // Armed or parked and never raised: the waiter woke on something that opened no span,
        // so there is no t0 to subtract. Counted, or the loss reads as a sweep that ran fewer
        // passes.
        if (mode == E2E_ARMED or mode == E2E_PARKED)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EBUSY;
        }
        uint64_t const dns = end - g_e2e_t0;
        // Over 4.29 s: wider than the ns columns carry, and a stall rather than a wake. Capping
        // it instead lands the stall in the distribution as a sample, the capped value being one
        // acc_add accepts.
        if (dns > 0xFFFFFFFFull)
        {
            g_e2e_dropped = g_e2e_dropped + 1;
            return -KOS_EBUSY;
        }
        uint32_t d = 1; // 0 is what a frozen clock reads; a real sub-tick span is not that
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
        // The mechanism goes in the LITERAL: a %s would be a seventh argument on a line the
        // stack-descent gate's chain runs through.
        kprintf_paced("  e2e-probe: line=%u closed=%u dropped=%u"
                " tare=%u/%u ns  (min/avg, n=%u)\n",
                static_cast<unsigned>(g_e2e_line), static_cast<unsigned>(g_e2e_closed),
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
