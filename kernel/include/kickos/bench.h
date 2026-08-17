// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_BENCH instrumentation: the per-arch cycle source, plus a per-phase
// min/max/sum/count accumulator so a kernel path can be bracketed where it runs.
// Everything here compiles to nothing when KICKOS_BENCH is 0.

#ifndef KICKOS_BENCH_H
#define KICKOS_BENCH_H

#include <stdint.h>

namespace kickos
{
    // Phase identifiers. Declared unconditionally, outside the KICKOS_BENCH guard:
    // KICKOS_BENCH_SPAN discards its arguments in a non-bench build, so a name that
    // does not exist would still compile and a typo would survive to the bench build.
    //
    // PH_NULL is an EMPTY bracket taken once per round trip. Its min is the instrument's
    // own cost, so every other phase's figure is (phase - k * PH_NULL) for the k brackets
    // nested inside it. It is not optional: without it no phase number can be believed.
    enum BenchPhase : uint32_t
    {
        PH_NULL = 0,
        PH_CALL_TOTAL,
        PH_CALL_VALIDATE,
        PH_CALL_LOCKED,
        PH_CALL_RESOLVE,
        PH_CALL_PROBE,
        PH_CALL_COPY,
        PH_CALL_MINT,
        PH_CALL_DONATE,
        PH_CALL_PARK,
        PH_CALL_WAKE,
        PH_CALL_RESUME,
        // The slowpath arm keeps its OWN phases. The two arms do different work, and a
        // shared accumulator would let one slowpath sample move the fastpath's min with
        // nothing in the table saying it had. n == 0 on these two is the evidence that
        // the sweep really did run entirely on the fastpath.
        PH_CALL_SLOW_DONATE,
        PH_CALL_SLOW_PARK,
        PH_REPLY_TOTAL,
        PH_REPLY_VALIDATE,
        PH_REPLY_LOCKED,
        PH_REPLY_LOOKUP,
        PH_REPLY_COPY,
        PH_REPLY_FUNNEL,
        PH_REPLY_WAKE,
        PH_KTIME_REARM,
        PH_MPU_APPLY,
        PH_SWITCH_TO,
        PH_COUNT
    };
}

#if defined(KICKOS_BENCH) && KICKOS_BENCH

#if defined(__riscv)
// RISC-V cycle source, defined in arch_rv32imac.cc. Default null -> `rdcycle` CSR
// (qemu-virt); a core that traps on it (the ESP32-C6) points it at a free-running MMIO
// counter (CLINT MTIME). Declared at global scope so it keeps C linkage (see switch.S).
extern "C" volatile uint32_t* g_bench_cycle_src;
#endif

// always_inline is load-bearing, not a hint: at -Os GCC emits an out-of-line copy of a
// body this small, and the call would be charged to whichever phase the bracket wraps.
// Same spelling and same reason as KICKOS_ATOMIC_INLINE in <kickos/sys/atomic.h>.
#define KICKOS_BENCH_INLINE inline __attribute__((always_inline))

namespace kickos
{
    // Per-arch free-running cycle counter. Returns 0 where the arch has none (Cortex-M0,
    // sim): every delta is then 0 and the phase table reads all zeroes, which is the
    // honest answer rather than a wrong one.
#if defined(__riscv)
    KICKOS_BENCH_INLINE uint32_t bench_cyccnt()
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
    KICKOS_BENCH_INLINE uint32_t bench_cyccnt()
    {
        return *reinterpret_cast<volatile uint32_t*>(0x00094290u) << 5;
    }
#elif defined(__XTENSA__)
    KICKOS_BENCH_INLINE uint32_t bench_cyccnt()
    {
        uint32_t v;
        __asm volatile("rsr.ccount %0" : "=a"(v)); // LX6 cycle counter @ CPU clock
        return v;
    }
#elif defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
    KICKOS_BENCH_INLINE uint32_t bench_cyccnt()
    {
        return *reinterpret_cast<volatile uint32_t*>(0xE0001004u); // DWT CYCCNT
    }
#else
    KICKOS_BENCH_INLINE uint32_t bench_cyccnt() { return 0; } // armv6m (no DWT) / sim
#endif

    // Single-writer kernel state, every write under IrqLock or in one thread, so the
    // accumulators are plain types. <kickos/sys/atomic.h> exposes no read-modify-write
    // anyway and tests/static/check_atomic_rmw.sh refuses one.
    void bench_phase_add(uint32_t phase, uint32_t delta);
    void bench_reset(); // switch accumulator AND every phase accumulator

    // Both print from the KERNEL, in thread context and outside any IrqLock: that is what
    // lets the syscall carry no out-pointer and copy no struct across the boundary.
    uint32_t bench_switch_print(); // returns the switch sample count
    void bench_phase_print();

    uint32_t bench_core_hz();
    void bench_irq_setup(int line);
    uint32_t bench_irq_once(int line);
    uint32_t bench_irq_masked_once(int line, uint32_t span_bytes);
}

// A bracket is a MARK and a SPAN over the same variable name. Both vanish in a non-bench
// build, so the marked variable must never be read by anything but its SPAN.
//
// A bracket must not contain a context switch, or it measures the whole rest of the
// round trip instead of its own body. On armv7m, rv32imac and rxv3 a switch requested
// under IrqLock only PENDS (PendSV / msip / SWINT) and fires when the mask lifts, so a
// bracket that closes before the lock does is safe. On the LX6 and the sim arch_switch
// SWAPS INLINE from thread context, and any bracket spanning sched::wake, wq_block or
// switch_to there closes only when this thread is next resumed.
#define KICKOS_BENCH_MARK(var) uint32_t const var = ::kickos::bench_cyccnt()
#define KICKOS_BENCH_SPAN(phase, var) \
    ::kickos::bench_phase_add((phase), ::kickos::bench_cyccnt() - (var))

namespace kickos
{
    // For a body with more than one exit, where a MARK/SPAN pair would have to be
    // repeated at each return.
    class BenchScope
    {
    public:
        explicit BenchScope(uint32_t phase)
            : phase_(phase)
            , start_(bench_cyccnt())
        {
        }
        ~BenchScope() { bench_phase_add(phase_, bench_cyccnt() - start_); }
        BenchScope(BenchScope const&) = delete;
        BenchScope& operator=(BenchScope const&) = delete;

    private:
        uint32_t phase_;
        uint32_t start_;
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

#endif

#endif
