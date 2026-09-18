// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_BENCH instrumentation: the per-arch cycle source, the named DISTRIBUTIONS
// (min/max/sum/count plus a histogram) and the per-phase min/max/sum/count accumulators, one
// row of each per kernel core. The brackets compile to nothing when KICKOS_BENCH is 0; the
// phase and distribution names below are declared either way.

#ifndef KICKOS_BENCH_H
#define KICKOS_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>

// The WIDTH of the tick type the brackets subtract, which is the width of the arch's counter
// where it has one. It must track the bench_cyccnt arms below; the 64-bit arms return through
// BenchTick{} so a disagreement in the narrowing direction refuses to compile.
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
    // The named distributions. Each slot costs one accumulator plus, where the board keeps a
    // histogram, 672 bytes PER KERNEL CORE.
    //
    // LOCK_HOLD is the outermost IrqLock bracket, which is not the same object on both
    // postures: at one kernel core klock_enter and klock_leave are empty, so the sample is the
    // interrupt-masked window and nothing else; above one core it is that window with the
    // cross-core kernel lock, and the wait for it, inside. Its per-core MAX is the longest
    // interrupt-masked window that core took under an IrqLock.
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
        // EVERY SLOT FROM HERE DOWN IS FILLED BY A SWEEP, not by the running workload, and is
        // printed by the op that ran that sweep. bench_dist_print stops at BD_IRQ_ENTRY: a
        // swept slot printed with the others would report the window before its sweep.
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
        // The two halves of PH_CALL_MINT, nested inside it: a capability-table mint and a
        // write into the receiver's user memory. A MINT - MINT_CAP - MINT_INFO residual larger
        // than their two brackets plus the assert between them means the split is misplaced.
        PH_CALL_MINT_CAP,
        PH_CALL_MINT_INFO,
        PH_CALL_DONATE,
        PH_CALL_PARK,
        PH_CALL_WAKE,
        PH_CALL_RESUME,
        // The slowpath arm keeps its OWN phases down to the two composites: a shared
        // accumulator lets one slowpath sample move the fastpath's min with nothing in the
        // table saying so.
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
        // The wake path, in execution order. Every phase below is fed by BOTH sides of a
        // round trip and by every other wake and reschedule in the system, so n is what says
        // whether a min came from the path being measured. SWITCH_TO is the composite over the
        // four that follow it; the rest are leaves.
        PH_WAKE_UNPARK,
        PH_PICK_NEXT,
        PH_SWITCH_TO,
        PH_SWITCH_BOOK,
        PH_MPU_APPLY,
        // The DEFERRED half. arch_mpu_apply only stashes on a pending-switch arch; the
        // switch epilogue programs the hardware afterwards, from assembly, where no
        // KICKOS_BENCH_SPAN can reach. Fed through kickos_bench_mpu_commit, so it is a
        // LEAF and corrects by PH_NULL. Zero on a backend whose commit is empty.
        PH_MPU_COMMIT,
        PH_KTIME_REARM,
        // A leaf only where arch_switch PENDS (armv7m, rv32imac, rxv3). On the LX6 and the
        // sim it swaps inline, and this one closes when the thread is next resumed.
        PH_ARCH_SWITCH,
        PH_COUNT
    };
}

#if defined(KICKOS_BENCH) && KICKOS_BENCH

// kickos_kernel_core(), which is NOT arch_cpu_id(): the rows below are the kernel's length and
// an AMP peer's machine identity would index past them.
#include <kickos/instance.h>
#include <kickos/sys/atomic.h>

#if defined(__riscv) && __riscv_xlen == 32
// RISC-V cycle source, defined in arch_rv32imac.cc. Null means the `rdcycle` CSR
// (qemu-virt); a core that traps on it (the ESP32-C6) points it at a free-running MMIO
// counter (CLINT MTIME). Global scope, for the C linkage switch.S needs.
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

    // Per-arch free-running cycle counter. Returns 0 where the arch has none (Cortex-M0,
    // sim), so every delta is 0 and the whole phase table reads zero.
    //
    // THE LX6 ALONE COUNTS 32 BITS AND SWAPS arch_switch INLINE. A span that rides across the
    // swap and closes on the next resume still wraps there, and nothing here can tell that
    // from a real reading.
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

    // Single-writer state: a bracket writes ITS OWN CORE'S row and no other, which is what
    // lets the accumulators be plain types at any core count.
    //
    // A DELTA WIDER THAN THE ROW'S 32-BIT STATISTICS IS COUNTED APART AND ENTERS NO STATISTIC,
    // and it is never a cost. Two things produce one, both on a bracket that encloses an
    // inline swap: a span closed on the thread's next resume, which times a park; and, above
    // one kernel core, a span opened on one core and closed on another, which subtracts two
    // cycle counters with no common zero and can read as very nearly 2^64.
    void bench_phase_add(uint32_t phase, BenchTick delta);
    void bench_dist_add(uint32_t dist, BenchTick delta);
    uint32_t bench_dist_count(uint32_t dist); // THIS core's row alone, for the probe below
    // `fast_taken` is ipc_fast_taken_count() as the caller read it: the window's baseline for
    // the swaps the trap-handler IPC fastpath performs without reaching the deferred switcher,
    // which are therefore not in BD_SWITCH.
    void bench_reset(uint32_t fast_taken); // every core's distributions AND every phase accumulator

    // Both print from the KERNEL, in thread context and outside any IrqLock, and both
    // AGGREGATE the rows there; above one core each also prints its per-core lines.
    uint32_t bench_dist_print(uint32_t fast_taken); // returns the switch sample count summed over the cores
    void bench_phase_print();

    // Three nested IrqLocks in one line of output: the counts must rise by exactly one, on the
    // calling core. Zero says the bracket never accumulates or accumulated on another core's
    // row; three says it samples every nesting level instead of the outermost.
    void bench_lock_probe_print();

#if KICKOS_KERNEL_CORES > 1
    // Drives BD_DOORBELL with `rounds` raise-and-rendezvous rounds over every peer, run from
    // `core`: the calling thread is placed there first, so a four-core board gets four
    // initiators rather than whichever core the reporter happened to sit on. Returns the rounds
    // it ran, 0 for a core the calling thread may not run on.
    uint32_t bench_doorbell_probe_print(uint32_t core, uint32_t rounds);
#endif

    // Rate of the counter bench_cyccnt() reads, which is NOT always the core clock: a part
    // whose counter runs off something else states KICKOS_CHIP_CYCCNT_HZ. 0 means no rate
    // converts a reading into time, and a caller must then report cycles alone.
    uint64_t bench_cyccnt_hz();

    // The tier-2 entry-latency line: attached, cleared and unmasked once, and remembered, so
    // the two sweeps below take no line argument and cannot be pointed at a second one.
    // A REFUSED ATTACH IS REPORTED: the line then keeps the null-object default, every inject
    // is counted as spurious, and the sweep below reports the same clean zero a line the
    // controller cannot raise does.
    int bench_irq_setup(int line);
    // Each fills its distribution from scratch, prints one row and answers the sample count.
    // A 0 means the controller never raised the line, which is not the same finding as a slow
    // line and prints the same without this count.
    uint32_t bench_irq_sweep(uint32_t samples);
    uint32_t bench_irq_wcase_sweep(uint32_t span_index, uint32_t samples);
    uint32_t bench_irq_wcase_spans();

    // The end-to-end span. THREE CALLERS AND THEY ARE NOT INTERCHANGEABLE: the WAITER arms and
    // closes, the RAISER raises, and the close refuses any thread but the armed waiter, so a
    // span closed anywhere but in the woken thread is dropped rather than counted.
    int bench_e2e_arm(int line);
    // The waiter's own park publication, called on its way into the block and under the same
    // kernel lock the ISR's post has to take, so a line injected past it cannot be delivered
    // to a thread that is still running.
    void bench_e2e_park_mark();
    // -KOS_EBUSY until the waiter has published its park: the raise would otherwise land on a
    // line whose owner is still running and the sample would measure a fast-path wait return.
    int bench_e2e_raise();
    // Opens a span with NO raise, so the close that follows prices the instrument's own tail:
    // the return to userspace, the device read and the trap back in. It is the floor every
    // end-to-end figure sits above.
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
    // The outermost IrqLock bracket's state. `start` is PER CORE and `depth` RIDES THE FRAME
    // across a swap, and the split is what makes the sample a core's masked window rather than
    // a thread's: an inline-swapping backend (arm64, rv64, LX6, sim) holds the kernel lock
    // across arch_switch, so a bracket opened by one thread on a core is closed by the thread
    // that resumes there, and that thread may itself be arriving from another core. The core's
    // interrupts stayed masked the whole time, which is the window being reported; the nesting
    // depth belongs to the thread, so switch_to detaches and re-attaches it exactly as it does
    // the kernel lock's.
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

    // Written by the tier-1 ISR, read by the close: the core that took the interrupt. NOT a
    // core index until an ISR has run, and the close drops a span that still reads NONE. No
    // interrupt arrived there, so there was no wake to time.
    //
    // RELAXED AND NOT A PUBLICATION POINT. The order it needs is already there: this ISR runs
    // ahead of the sem_post that wakes the waiter, that post holds the kernel lock, and the
    // waiter's core acquires the same lock to pick the thread up (kernel/sync/klock.cc,
    // arch_kernel_lock / arch_kernel_unlock).
    constexpr uint32_t BENCH_CORE_NONE = 0xFFFFFFFFu;
    extern Atomic<uint32_t, Order::RELAXED> g_bench_e2e_isr_core;
    KICKOS_BENCH_INLINE void bench_e2e_isr_mark()
    {
        g_bench_e2e_isr_core = kickos_kernel_core();
    }

    // always_inline on both, for the reason every other body in this file carries it: at -Os
    // an out-of-line destructor is a node the console-reach and stack-descent graphs cannot
    // see through, and it charges its call to whichever phase the bracket wraps.
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

// The IrqLock bracket and the detach/attach switch_to owes it. Named through macros so the
// non-bench build carries no reference to a declaration it does not have.
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
