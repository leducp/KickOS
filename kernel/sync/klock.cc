// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/klock.h>

#if KICKOS_KERNEL_CORES > 1

#include <kickos/bench.h>
#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>

namespace kickos
{
    namespace
    {
        // A53 cache line, so no two cores write one.
        constexpr size_t KLOCK_CACHE_LINE = 64u;

        struct alignas(KLOCK_CACHE_LINE) KlockRow
        {
            // Nesting depth. Written by the owning core alone, under that core's own
            // interrupt mask.
            uint32_t depth;
            // Set while this core holds the lock at depth zero and a switch owes the
            // release. The bracket must not release it while this is set.
            uint32_t owed;
        };
        static_assert(sizeof(KlockRow) % KLOCK_CACHE_LINE == 0,
                      "a row shorter than a line would share one with the next writer");

        KlockRow g_row[KICKOS_KERNEL_CORES] = {};

        using Resched = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        // One row per core, each row written by that core alone.
        struct alignas(KLOCK_CACHE_LINE) ReschedRow
        {
            Resched seq[KICKOS_KERNEL_CORES];
        };
        static_assert(sizeof(ReschedRow) % KLOCK_CACHE_LINE == 0,
                      "a row shorter than a line would share one with the next writer");

        // g_asked[i].seq[t]: reschedules core i has owed core t. Written by i, read by t.
        // g_took[t].seq[i]: how far core t has consumed i's. Written by t, read by i.
        ReschedRow g_asked[KICKOS_KERNEL_CORES] = {};
        ReschedRow g_took[KICKOS_KERNEL_CORES] = {};

        // klock_drop releases unconditionally; a release taken where this core does not hold
        // the lock hands it to a ticket nobody drew, so no draw ever matches and every core
        // spins forever. The check must test this core specifically: the ticket counters read
        // as held while a different core holds.
        inline void release(void)
        {
            KICKOS_DEBUG_ASSERT(::arch_kernel_lock_held() != 0);
            arch_kernel_unlock();
        }

        inline __attribute__((always_inline)) bool resched_owed_on(uint32_t me)
        {
            for (uint32_t from = 0; from < KICKOS_KERNEL_CORES; from++)
            {
                if (g_asked[from].seq[me].load() != g_took[me].seq[from].load())
                {
                    return true;
                }
            }
            return false;
        }

        // The end of a span that booked no swap, or of the swap's own: what the span staged is
        // published, and a reschedule a poll absorbed is raised again, since only the dispatch
        // that enters the scheduler consumes the cell.
        inline __attribute__((always_inline)) void span_end(void)
        {
            uint32_t me = kickos_kernel_core();
            if (sched_flush(me))
            {
                // Read again rather than held across the call: held, it is a spill on lx6,
                // which deepens every trap chain that ends a span.
                me = kickos_kernel_core();
            }
            if (resched_owed_on(me))
            {
                arch_ipi_raise(1u << me);
            }
        }

        inline __attribute__((always_inline)) void drop(void)
        {
            KlockRow& r = g_row[kickos_kernel_core()];
            r.depth = 0;
            r.owed = 0;
            release();
        }
    }

    void klock_enter(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        if (r.depth == 0 and r.owed == 0)
        {
            // The wait sample, charged to the hold span enclosing it: the accumulator call
            // sits inside the masked window the IrqLock bracket is timing.
            KICKOS_BENCH_MARK(bw);
            arch_kernel_lock();
            KICKOS_BENCH_DIST_SPAN(BD_LOCK_WAIT, bw);
            KICKOS_BENCH_ACQUIRED(0);
        }
        r.depth = r.depth + 1u;
    }

    void klock_leave(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        r.depth = r.depth - 1u;
        if (r.depth == 0 and r.owed == 0)
        {
            release();
            span_end();
        }
    }

#if KICKOS_DEBUG
    bool klock_exclusion_held(void)
    {
        KlockRow const& r = g_row[kickos_kernel_core()];
        return r.depth != 0 or r.owed != 0;
    }

    uint32_t klock_depth(void)
    {
        return g_row[kickos_kernel_core()].depth;
    }
#endif

    uint32_t klock_detach(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        uint32_t const depth = r.depth;
        r.depth = 0;
        r.owed = 1u;
        return depth;
    }

    void klock_attach(uint32_t depth)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        // owed still set means the swap was merely booked and this core never let go, so an
        // acquire here would spin on a word it holds itself.
        if (r.owed == 0)
        {
            KICKOS_BENCH_MARK(bw);
            arch_kernel_lock();
            KICKOS_BENCH_DIST_SPAN(BD_LOCK_WAIT, bw);
            KICKOS_BENCH_ACQUIRED(1);
        }
        r.depth = depth;
    }

    void klock_drop(void)
    {
        drop();
    }

    // Runs once the outgoing frame is parked and this core stands on the incoming one; the
    // unlock's release publishes that parked frame.
    // The other half of klock_leave's arm, not a duplicate of it: a swap booked from an
    // interrupt runs at the exception exit, so klock_detach left `owed` set and the klock_leave
    // that follows the booking releases nothing and raises nothing. This is the release that
    // ends that span, the only one a deferred backend reaches.
    extern "C" void kickos_switch_unlock(void)
    {
        KICKOS_BENCH_SWITCHED();
        drop();
        span_end();
    }

    // The one body that gives a cross-core raise scheduling meaning: a second publisher of
    // this cell would put the ask ahead of a raise nobody ordered it against.
    void klock_resched_ask(uint32_t cores)
    {
        // Sets the cell before the raise, for peers alone: the raise is an edge a poll may
        // absorb instead of the vector, and the cell is what outlives it. A bare raise: a
        // reschedule owes no rendezvous answer.
        uint32_t const peers = cores & ~(1u << kickos_kernel_core());
        ::kickos_kernel_core_resched_owe(peers);
        arch_ipi_raise(peers);
    }

    // No raise here: the release that ends this core's lock span carries it, via
    // klock_leave's depth-zero arm or kickos_switch_unlock when a booked swap left the lock
    // owed to it. A raise from inside the bracket would be taken against a core that still
    // holds it.
    void klock_resched_self(void)
    {
        ::kickos_kernel_core_resched_owe(1u << kickos_kernel_core());
    }

    // The reschedule a raise carries, held as state rather than as the raise itself
    // (arch/include/kickos/arch/arch.h). Every cell has exactly one writer: an asked word
    // only by the core owing, a took word only by the core consuming.
    extern "C" void kickos_kernel_core_resched_owe(uint32_t cores)
    {
        uint32_t const me = kickos_kernel_core();
        for (uint32_t to = 0; to < KICKOS_KERNEL_CORES; to++)
        {
            if ((cores & (1u << to)) != 0)
            {
                // Single writer, so a load and a store rather than an increment.
                g_asked[me].seq[to] = g_asked[me].seq[to].load() + 1u;
            }
        }
    }

    extern "C" int kickos_kernel_core_resched_owed(void)
    {
        if (resched_owed_on(kickos_kernel_core()))
        {
            return 1;
        }
        return 0;
    }

    // Stores the sequence it read rather than a blanket clear, so a peer that owes another
    // reschedule between this load and this store is owed again instead of answered.
    extern "C" int kickos_kernel_core_resched_take(void)
    {
        uint32_t const me = kickos_kernel_core();
        int stood = 0;
        for (uint32_t from = 0; from < KICKOS_KERNEL_CORES; from++)
        {
            uint32_t const asked = g_asked[from].seq[me].load();
            if (asked != g_took[me].seq[from].load())
            {
                g_took[me].seq[from] = asked;
                stood = 1;
            }
        }
        if (stood != 0)
        {
            KICKOS_BENCH_RESCHED_TAKE();
        }
        return stood;
    }
}

#endif
