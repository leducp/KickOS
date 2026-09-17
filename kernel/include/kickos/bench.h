// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-core benchmark distributions, histograms and phase accumulators.
// Instrumentation is disabled when KICKOS_BENCH is zero.

#ifndef KICKOS_BENCH_H
#define KICKOS_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>

// Match BenchTick to the counter width used by bench_cyccnt.
#if (defined(__riscv) && __riscv_xlen == 64) || defined(__aarch64__) || defined(__x86_64__)
#define KICKOS_BENCH_TICK_BITS 64
#else
#define KICKOS_BENCH_TICK_BITS 32
#endif

#if KICKOS_KERNEL_CORES > 1
// A53 cache line, so no two cores write one.
#define KICKOS_BENCH_CACHE_LINE 64u
#define KICKOS_BENCH_PERCORE_ALIGNED alignas(KICKOS_BENCH_CACHE_LINE)
#else
#define KICKOS_BENCH_PERCORE_ALIGNED
#endif

namespace kickos
{
    // Each distribution uses an accumulator and optionally a 672-byte histogram
    // per kernel core. LOCK_HOLD measures the outermost IrqLock interval, including
    // interrupt masking and, on SMP, kernel-lock acquisition and hold time.
    enum BenchDist : uint32_t
    {
        BD_SWITCH = 0,
        BD_LOCK_HOLD,
#if KICKOS_KERNEL_CORES > 1
        // The spin in arch_kernel_lock.
        BD_LOCK_WAIT,
        // ONE WHOLE ROUND: this core's request cell bumped for every peer, the one raise, the
        // far sides servicing it, and this core observing the LAST of their answers.
        BD_DOORBELL,
#endif
        // The following slots are populated and printed by explicit sweeps.
        // Exclude them from ordinary workload reporting.
        BD_IRQ_ENTRY,
        // ONE slot for the four masked span sizes, cleared and re-reported per span, so its
        // count is one span's and never the sweep's.
        BD_IRQ_WCASE,
        // The raise, through delivery and dispatch and the wake, to the woken userspace
        // thread's first read of the device window it holds. Split on whether that thread ran
        // on the core that took the interrupt.
        BD_IRQ_E2E_LOCAL,
#if KICKOS_KERNEL_CORES > 1
        BD_IRQ_E2E_CROSS,
#endif
        BD_COUNT
    };

    // Keep phase names available in non-benchmark builds.
    // PH_NULL measures two counter reads. PH_NEST also includes the nested
    // accumulator call, which runs after its own closing read. Corrections are:
    //   leaf: leaf - PH_NULL
    //   composite: composite - PH_NULL - k * (PH_NEST - PH_NULL)
    // k is the number of nested brackets per sample, including deeper nesting.
    // Conditional paths change k; inline context switches can make it unbounded.
    enum BenchPhase : uint32_t
    {
        PH_NULL = 0,
        PH_NEST,
        PH_CALL_TOTAL,
        PH_CALL_VALIDATE,
        PH_CALL_LOCKED,
        PH_CALL_RESOLVE,
        PH_CALL_PEEK,
        PH_CALL_PROBE,
        PH_CALL_POP,
        PH_CALL_COPY,
        PH_CALL_MINT,
        // Nested PH_CALL_MINT phases: capability allocation and user-memory write.
        PH_CALL_MINT_CAP,
        PH_CALL_MINT_INFO,
        PH_CALL_DONATE,
        PH_CALL_PARK,
        PH_CALL_WAKE,
        PH_CALL_RESUME,
        // Keep slowpath samples separate from fastpath minima.
        PH_CALL_SLOW_TOTAL,
        PH_CALL_SLOW_LOCKED,
        PH_CALL_SLOW_DONATE,
        PH_CALL_SLOW_PARK,
        // Receive phases shared by plain and fused receives. Recorded only when parking;
        // receiving a queued message returns before these measurements close.
        PH_RECV_LOCKED,
        PH_RECV_RESOLVE,
        PH_RECV_SCAN,
        PH_RECV_PARK,
        // TOTAL measures standalone replies. LOCKED measures the shared reply body
        // for both standalone and fused calls. Both require a local caller to be answered.
        PH_REPLY_TOTAL,
        PH_REPLY_VALIDATE,
        PH_REPLY_LOCKED,
        PH_REPLY_LOOKUP,
        PH_REPLY_COPY,
        PH_REPLY_FUNNEL,
        PH_REPLY_WAKE,
        // Fused reply-receive total, including deferred wake completion. Closes before
        // IrqLock releases, excluding the wait on deferred-switch architectures.
        // Inline-switch architectures include suspension inside wq_block.
        // Reply refusals are not recorded. REPLY_LOCKED and RECV_LOCKED measure its parts.
        PH_REPLY_RECV_TOTAL,
        // Post-resume work for parked receives: resume barrier, result read, and
        // notification consumption. Nonparking paths do not contribute.
        // Neither this phase nor TOTAL includes notification-mask write-back.
        PH_REPLY_RECV_TAIL,
        // Wake/switch phases include all scheduling activity, so check sample counts.
        // SWITCH_TO is composite. Its nested bracket count depends on deferred MPU
        // commit support and KICKOS_LIBC_REENT.
        PH_WAKE_UNPARK,
        PH_PICK_NEXT,
        PH_SWITCH_TO,
        PH_SWITCH_BOOK,
        PH_MPU_APPLY,
        // Deferred MPU programming, recorded from assembly through kickos_bench_mpu_commit.
        // A leaf phase corrected by PH_NULL; absent when commit does no work.
        PH_MPU_COMMIT,
        // Libc state setup, including first-use initialization, under KICKOS_LIBC_REENT.
        PH_REENT_SEAT,
        PH_KTIME_REARM,
        // A leaf only where arch_switch PENDS (armv7m, rv32imac, rxv3). On the LX6 and the
        // sim it swaps inline, and this one closes when the thread is next resumed.
        PH_ARCH_SWITCH,
        PH_COUNT
    };
}

#if defined(KICKOS_BENCH) && KICKOS_BENCH

// Use kickos_kernel_core(), not arch_cpu_id(): an AMP machine core ID
// can exceed this kernel's row count.
#include <kickos/instance.h>
#include <kickos/sys/atomic.h>

#if defined(__riscv) && __riscv_xlen == 32
// RV32 cycle source: null selects rdcycle; otherwise use the MMIO counter.
// Global C linkage is required by switch.S.
extern "C" volatile uint32_t* g_bench_cycle_src;
#endif

// always_inline is load-bearing, not a hint: at -Os an out-of-line copy of a body this small
// charges its call to whichever phase the bracket wraps.
#define KICKOS_BENCH_INLINE inline __attribute__((always_inline))

namespace kickos
{
#if KICKOS_BENCH_TICK_BITS == 64
    using BenchTick = uint64_t;
#else
    using BenchTick = uint32_t;
#endif

    // Return the architecture counter or zero if unavailable.
    // LX6 has a 32-bit counter, so intervals spanning inline switches can wrap.
#if defined(__riscv) && __riscv_xlen == 32
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        if (g_bench_cycle_src != nullptr)
        {
            return *g_bench_cycle_src;
        }
        uint32_t v;
        __asm volatile("rdcycle %0" : "=r"(v));
        return v;
    }
#elif defined(__riscv)
    // rv64 runs in S-mode, where this CSR reads only because startup.S set mcounteren.CY and
    // refused by name when the bit did not stick.
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        uint64_t v;
        __asm volatile("rdcycle %0" : "=r"(v));
        return BenchTick{v};
    }
#elif defined(__aarch64__)
    // The PMU cycle counter, enabled per core by kickos_armv8a_percore_init.
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        uint64_t v;
        __asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
        return BenchTick{v};
    }
#elif defined(__x86_64__)
    // LFENCE first, as apic_x86_64.cc's own reader does: the counter read is a measurement
    // boundary and rdtsc is not ordered against the instructions around it.
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        uint32_t lo;
        uint32_t hi;
        __asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi)::"memory");
        return BenchTick{(static_cast<uint64_t>(hi) << 32) | lo};
    }
#elif defined(__RX__)
    // CMTW1 free-running counter (7.5 MHz), <<5 (x32) to match switch.S's ICLK-cycle scaling.
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        return *reinterpret_cast<volatile uint32_t*>(0x00094290u) << 5;
    }
#elif defined(__XTENSA__)
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        uint32_t v;
        __asm volatile("rsr.ccount %0" : "=a"(v)); // LX6 cycle counter @ CPU clock
        return v;
    }
#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt()
    {
        return *reinterpret_cast<volatile uint32_t*>(0xE0001004u); // DWT CYCCNT
    }
#else
    KICKOS_BENCH_INLINE BenchTick bench_cyccnt() { return 0; } // armv6m (no DWT) / sim
#endif

    // Each core writes only its own row. Count intervals beyond 32 bits as
    // saturated and exclude them from statistics. Inline switches can include
    // suspension or migration between unsynchronized counters.
    void bench_phase_add(uint32_t phase, BenchTick delta);
    void bench_dist_add(uint32_t dist, BenchTick delta);
    uint32_t bench_dist_count(uint32_t dist); // THIS core's row alone, for the probe below
    // Capture the fastpath-switch baseline; these switches bypass BD_SWITCH.
    void bench_reset(uint32_t fast_taken); // every core's distributions AND every phase accumulator

    // Both print from the KERNEL, in thread context and outside any IrqLock, and both
    // AGGREGATE the rows there; above one core each also prints its per-core lines.
    uint32_t bench_dist_print(uint32_t fast_taken); // returns the switch sample count summed over the cores
    void bench_phase_print();

    // Three nested IrqLocks must produce one sample on the calling core.
    void bench_lock_probe_print();

#if KICKOS_KERNEL_CORES > 1
    // Run raise-and-wait rounds from the requested allowed core.
    // Return completed rounds, or zero if placement is disallowed.
    uint32_t bench_doorbell_probe_print(uint32_t core, uint32_t rounds);
#endif

    // Report attachment failure so zero samples cannot look like unsupported injection.
    uint64_t bench_cyccnt_hz();

    // Attach, clear and unmask the entry-latency IRQ once for both sweeps.
    // Report attach failure; the null handler then counts injections as spurious
    // and the sweep reports zero, as for an unsupported software raise.
    int bench_irq_setup(int line);
    // Reset, populate and print a distribution; return its sample count.
    uint32_t bench_irq_sweep(uint32_t samples);
    uint32_t bench_irq_wcase_sweep(uint32_t span_index, uint32_t samples);
    uint32_t bench_irq_wcase_spans();

    // Only the waiter may arm and close the interval; the raiser only injects.
    // Reject closes from other threads.
    int bench_e2e_arm(int line);
    // Publish the park under the same kernel lock used by the ISR wake.
    void bench_e2e_park_mark();
    // -KOS_EBUSY until the waiter has published its park: the raise would otherwise land on a
    // line whose owner is still running and the sample would measure a fast-path wait return.
    int bench_e2e_raise();
    // Measure the return/read/trap overhead without injecting an interrupt.
    int bench_e2e_tare();
    int bench_e2e_close();
    void bench_e2e_print(uint32_t asked);
}

// MARK and SPAN must use the same variable; both disappear in non-benchmark builds.
// Only SPAN may read the marked variable.
// A span crossing an inline context switch includes suspension, not just CPU
// work. ARMv7-M, RV32, and RXv3 defer switches until interrupts are restored.
// LX6 and sim switch inline. ARMv8-A, RV64, and x86-64 defer ISR switches
// but switch inline in thread context, including syscall paths.
// Migration can also mix unsynchronized counters. Oversized intervals count
// as saturated samples; LX6's 32-bit counter can instead wrap into statistics.
#define KICKOS_BENCH_MARK(var) ::kickos::BenchTick const var = ::kickos::bench_cyccnt()
#define KICKOS_BENCH_SPAN(phase, var) \
    ::kickos::bench_phase_add((phase), ::kickos::bench_cyccnt() - (var))
#define KICKOS_BENCH_DIST_SPAN(dist, var) \
    ::kickos::bench_dist_add((dist), ::kickos::bench_cyccnt() - (var))

namespace kickos
{
    // The timestamp is per core; nesting depth travels with the thread frame.
    // A switch can open and close the same core's masked interval on different
    // threads, including migrated ones. Detach and restore depth with the kernel lock.
    struct KICKOS_BENCH_PERCORE_ALIGNED BenchLockRow
    {
        uint32_t depth;
        BenchTick start;
    };
    extern BenchLockRow g_bench_lock[KICKOS_KERNEL_CORES];

    // A nested acquire costs an increment and takes no sample: the span reported is the
    // outermost one.
    KICKOS_BENCH_INLINE void bench_lock_open()
    {
        BenchLockRow& r = g_bench_lock[kickos_kernel_core()];
        if (r.depth == 0)
        {
            r.start = bench_cyccnt();
        }
        r.depth++;
    }

    // The accumulator call runs after the closing read, so it is invisible to its own sample.
    KICKOS_BENCH_INLINE void bench_lock_close()
    {
        BenchLockRow& r = g_bench_lock[kickos_kernel_core()];
        BenchTick const end = bench_cyccnt();
        if (r.depth == 0)
        {
            return;
        }
        r.depth--;
        if (r.depth == 0)
        {
            bench_dist_add(BD_LOCK_HOLD, end - r.start);
        }
    }

    // Zeroes the depth WITHOUT sampling and hands it to the caller's frame. `start` is left
    // alone: it describes this core's masked window, which the incoming thread continues.
    KICKOS_BENCH_INLINE uint32_t bench_lock_detach()
    {
        BenchLockRow& r = g_bench_lock[kickos_kernel_core()];
        uint32_t const depth = r.depth;
        r.depth = 0;
        return depth;
    }

    KICKOS_BENCH_INLINE void bench_lock_attach(uint32_t depth)
    {
        g_bench_lock[kickos_kernel_core()].depth = depth;
    }

    // For a caller that never returns to destroy its own bracket. Without it the depth never
    // falls back to zero and NO sample is ever taken again on that core.
    KICKOS_BENCH_INLINE void bench_lock_drop()
    {
        g_bench_lock[kickos_kernel_core()].depth = 0;
    }

    // ISR core ID; NONE means no handler ran. Relaxed access is sufficient:
    // the ISR wake and the waiter's scheduling pass synchronize through the kernel lock.
    constexpr uint32_t BENCH_CORE_NONE = 0xFFFFFFFFu;
    extern Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core;
    KICKOS_BENCH_INLINE void bench_e2e_isr_mark()
    {
        g_bench_e2e_isr_core = kickos_kernel_core();
    }

    // Keep brackets inline for accurate timing and stack/call-graph checks.
    class BenchScope
    {
    public:
        KICKOS_BENCH_INLINE explicit BenchScope(uint32_t phase)
            : phase_(phase)
            , start_(bench_cyccnt())
        {
        }
        KICKOS_BENCH_INLINE ~BenchScope() { bench_phase_add(phase_, bench_cyccnt() - start_); }
        BenchScope(BenchScope const&) = delete;
        BenchScope& operator=(BenchScope const&) = delete;

    private:
        uint32_t phase_;
        BenchTick start_;
    };
}

#else

#define KICKOS_BENCH_MARK(var) \
    do                         \
    {                          \
    } while (false)
#define KICKOS_BENCH_SPAN(phase, var) \
    do                                \
    {                                 \
    } while (false)
#define KICKOS_BENCH_DIST_SPAN(dist, var) \
    do                                    \
    {                                     \
    } while (false)

#endif

// Measure IrqLock and switch_to detach/attach. Macros remove references
// to benchmark declarations from other builds.
#if defined(KICKOS_BENCH) && KICKOS_BENCH
#define KICKOS_BENCH_LOCK_OPEN() ::kickos::bench_lock_open()
#define KICKOS_BENCH_LOCK_CLOSE() ::kickos::bench_lock_close()
#define KICKOS_BENCH_LOCK_DETACH(var) uint32_t const var = ::kickos::bench_lock_detach()
#define KICKOS_BENCH_LOCK_ATTACH(var) ::kickos::bench_lock_attach(var)
#define KICKOS_BENCH_LOCK_DROP() ::kickos::bench_lock_drop()
#define KICKOS_BENCH_E2E_ISR_MARK() ::kickos::bench_e2e_isr_mark()
#define KICKOS_BENCH_E2E_PARK_MARK() ::kickos::bench_e2e_park_mark()
#else
#define KICKOS_BENCH_LOCK_OPEN() \
    do                           \
    {                            \
    } while (false)
#define KICKOS_BENCH_LOCK_CLOSE() \
    do                            \
    {                             \
    } while (false)
#define KICKOS_BENCH_LOCK_DETACH(var) \
    do                                \
    {                                 \
    } while (false)
#define KICKOS_BENCH_LOCK_ATTACH(var) \
    do                                \
    {                                 \
    } while (false)
#define KICKOS_BENCH_LOCK_DROP() \
    do                           \
    {                            \
    } while (false)
#define KICKOS_BENCH_E2E_ISR_MARK() \
    do                              \
    {                               \
    } while (false)
#define KICKOS_BENCH_E2E_PARK_MARK() \
    do                               \
    {                                \
    } while (false)
#endif

#endif
