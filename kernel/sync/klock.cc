// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/klock.h>

#if KICKOS_KERNEL_CORES > 1

#include <kickos/instance.h>
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
    }

    void klock_enter(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        if (r.depth == 0 and r.owed == 0)
        {
            arch_kernel_lock();
        }
        r.depth = r.depth + 1u;
    }

    void klock_leave(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        r.depth = r.depth - 1u;
        if (r.depth == 0 and r.owed == 0)
        {
            arch_kernel_unlock();
            // A RAISE, NOT A SCHEDULER PASS, AND THE CELL IS LEFT STANDING: only the dispatch
            // that enters the scheduler consumes the cell, so a raise a poll absorbed must be
            // carried again.
            if (::kickos_kernel_core_resched_owed() != 0)
            {
                arch_ipi_resched_self();
            }
        }
    }

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
        // owed still set means the swap was merely BOOKED and this core never let go, so an
        // acquire here would spin on a word it holds itself.
        if (r.owed == 0)
        {
            arch_kernel_lock();
        }
        r.depth = depth;
    }

    void klock_drop(void)
    {
        KlockRow& r = g_row[kickos_kernel_core()];
        r.depth = 0;
        r.owed = 0;
        arch_kernel_unlock();
    }

    // Called by the arch from inside the swap, once the outgoing frame is parked and this
    // core stands on the incoming one; the unlock's release publishes that parked frame.
    extern "C" void kickos_switch_unlock(void)
    {
        klock_drop();
    }

    // THE ONE BODY THAT GIVES A CROSS-CORE RAISE SCHEDULING MEANING: a second publisher of
    // this cell would put the ask ahead of a raise nobody ordered it against.
    void klock_resched_ask(uint32_t cores)
    {
        // BEFORE THE RAISE, AND THE PEERS ALONE: the raise is an edge that the acquire loop's
        // poll may absorb instead of the vector, and the cell is what outlives it.
        ::kickos_kernel_core_resched_owe(cores & ~(1u << kickos_kernel_core()));
        arch_ipi_send(cores);
    }

    // The reschedule a raise carries, held as state rather than as the raise itself
    // (arch/include/kickos/arch/arch.h). EVERY CELL HAS EXACTLY ONE WRITER: an asked word
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
        uint32_t const me = kickos_kernel_core();
        for (uint32_t from = 0; from < KICKOS_KERNEL_CORES; from++)
        {
            if (g_asked[from].seq[me].load() != g_took[me].seq[from].load())
            {
                return 1;
            }
        }
        return 0;
    }

    // Stores the sequence it READ rather than a blanket clear, so a peer that owes another
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
        return stood;
    }
}

#endif
