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
// Keep per-core rows on separate cache lines.
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
        // Counts, not cycles: retried stores inside one ticket draw, and the tickets
        // already ahead of that ticket when it was drawn.
        BD_LOCK_DRAW,
        BD_LOCK_QUEUE,
        // Request all peers, raise the doorbell and wait for every reply.
        BD_DOORBELL,
#endif
        // The following slots are populated and printed by explicit sweeps.
        // Exclude them from ordinary workload reporting.
        BD_IRQ_ENTRY,
        // Reuse this slot for each masked span size, resetting it between spans.
        BD_IRQ_WCASE,
        // From IRQ raise to the woken userspace thread reading its device window.
        // Split by whether the waiter and handler ran on the same core.
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
        // Cost of an empty nested IrqLock, including instrumentation. Correct as a leaf.
        PH_NEST_LOCK,
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
        // A leaf on deferred-switch targets (armv7m, rv32imac, rxv3).
        // On LX6 and sim, the interval includes suspension until the thread resumes.
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

// Force inlining at -Os to keep call overhead out of measured phases.
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
    // S-mode access requires mcounteren.CY, checked by startup.S.
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
    // LFENCE orders RDTSC after preceding instructions.
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
    // Record the outermost lock interval and retain the release address of its maximum.
    void bench_lock_hold_add(BenchTick delta, void* site);
    uint32_t bench_dist_count(uint32_t dist); // Current core only.
    // Capture the fastpath-switch baseline; these switches bypass BD_SWITCH.
    void bench_reset(uint32_t fast_taken); // Reset distributions and phases on all cores.

    // Print in kernel thread context outside IrqLock. Include aggregate and per-core rows.
    uint32_t bench_dist_print(uint32_t fast_taken); // Return the aggregate switch count.
    void bench_phase_print();

    // Three nested IrqLocks must produce one sample on the calling core.
    void bench_lock_probe_print();

#if KICKOS_KERNEL_CORES > 1
    // Run raise-and-wait rounds from the requested allowed core.
    // Return completed rounds, or zero if placement is disallowed.
    uint32_t bench_doorbell_probe_print(uint32_t core, uint32_t rounds);
#endif

    uint64_t bench_cyccnt_hz();

    // Attach, clear and unmask the entry-latency IRQ for both sweeps.
    // Return attachment errors so they cannot look like unsupported injection.
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
    // Return -KOS_EBUSY until the waiter parks, excluding fast-path wait returns.
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

    // Only the outermost lock interval produces a sample.
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
            // Inlining makes level 0 the return address of the function holding the lock.
            bench_lock_hold_add(end - r.start, __builtin_return_address(0));
        }
    }

    // Transfer depth to the caller frame without sampling. Keep the per-core start
    // timestamp because the incoming thread continues the masked interval.
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

    // Clear depth for a caller that will not return to destroy its lock bracket.
    KICKOS_BENCH_INLINE void bench_lock_drop()
    {
        g_bench_lock[kickos_kernel_core()].depth = 0;
    }

    // ISR core ID; NONE means no handler ran. Relaxed access is sufficient:
    // the ISR wake and the waiter's scheduling pass synchronize through the kernel lock.
    constexpr uint32_t BENCH_CORE_NONE = 0xFFFFFFFFu;
    extern Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core;
    extern Atomic<int32_t, Order::RELAXED> g_bench_e2e_line;
    // THE ARMED LINE AND NO OTHER. The caller is the generic event trampoline, which is the
    // ISR of every driver-style claim, so a foreign line firing inside an open span would
    // otherwise restamp the core and flip the sample's local/cross classification.
    KICKOS_BENCH_INLINE void bench_e2e_isr_mark(int line)
    {
        if (line != g_bench_e2e_line)
        {
            return;
        }
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
#define KICKOS_BENCH_E2E_ISR_MARK(line) ::kickos::bench_e2e_isr_mark(line)
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
#define KICKOS_BENCH_E2E_ISR_MARK(line) \
    do                                  \
    {                                   \
        (void)(line);                   \
    } while (false)
#define KICKOS_BENCH_E2E_PARK_MARK() \
    do                               \
    {                                \
    } while (false)
#endif

#endif
