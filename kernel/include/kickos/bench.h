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
    // THE TWO CONTROLS, and they correct DIFFERENT things. KICKOS_BENCH_SPAN evaluates its
    // closing counter read as an ARGUMENT, so a bracket's own accumulator call runs AFTER
    // that read and is invisible to the bracket itself -- but it is fully inside whatever
    // span encloses it.
    //
    //   PH_NULL is an empty bracket. It prices the two counter reads, so a LEAF is
    //   (leaf - PH_NULL).
    //   PH_NEST is that same empty bracket with an enclosing span wrapped around it, so it
    //   prices what ONE COMPLETE NESTED BRACKET costs its parent: inner mark, inner closing
    //   read AND the inner accumulator call. A COMPOSITE holding k brackets at any depth is
    //   (composite - k * PH_NEST), which comes out as the sum of its children's CORRECTED
    //   values plus the parent's own unbracketed work.
    //
    // Correcting a composite with PH_NULL instead understates the charge by a whole
    // accumulator call: 1 cycle against about 57 on esp32-wroom. Neither is optional.
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
        // The two halves of PH_CALL_MINT, which share nothing: a capability-table mint
        // and a write into the receiver's user memory. They nest inside it, so
        // MINT - MINT_CAP - MINT_INFO is their two brackets plus the assert between them,
        // and a residual larger than that means the split is misplaced.
        PH_CALL_MINT_CAP,
        PH_CALL_MINT_INFO,
        PH_CALL_DONATE,
        PH_CALL_PARK,
        PH_CALL_WAKE,
        PH_CALL_RESUME,
        // The slowpath arm keeps its OWN phases, down to the two composites. The two arms
        // do different work, and a shared accumulator lets one slowpath sample move the
        // fastpath's min with nothing in the table saying it had -- which is exactly what
        // a shared CALL_LOCKED did, since the slowpath arm parks without ever reaching the
        // mint. Matching n against CALL_MINT is what catches a recurrence.
        PH_CALL_SLOW_TOTAL,
        PH_CALL_SLOW_LOCKED,
        PH_CALL_SLOW_DONATE,
        PH_CALL_SLOW_PARK,
        // The round trip's THIRD locked leg: the server's own recv, which parks it between
        // one reply and the next call. These close only on the PARKING arm -- the arm that
        // serves a queued sender returns from inside the scan, before the spans.
        PH_RECV_LOCKED,
        PH_RECV_RESOLVE,
        PH_RECV_SCAN,
        PH_RECV_PARK,
        PH_REPLY_TOTAL,
        PH_REPLY_VALIDATE,
        PH_REPLY_LOCKED,
        PH_REPLY_LOOKUP,
        PH_REPLY_COPY,
        PH_REPLY_FUNNEL,
        PH_REPLY_WAKE,
        // The wake path, in execution order. Every phase below is fed by BOTH sides of a
        // round trip and by every other wake and reschedule in the system, so n is what
        // says whether a min came from the path being measured.
        // SWITCH_TO is the composite over the four that follow it; the rest are leaves.
        PH_WAKE_UNPARK,
        PH_PICK_NEXT,
        PH_SWITCH_TO,
        PH_SWITCH_BOOK,
        PH_MPU_APPLY,
        PH_KTIME_REARM,
        // A leaf only where arch_switch PENDS (armv7m, rv32imac, rxv3). On the LX6 and the
        // sim it swaps inline, and this one closes when the thread is next resumed.
        PH_ARCH_SWITCH,
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
