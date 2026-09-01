// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The observable half of the seam kernel/irq/irq.cc is compiled against here. `g_core` is
// what arch_cpu_id answers and is PER THREAD, so an arm may run two cores at once; the trace
// records every controller and doorbell call in order, which is how an arm reads what a
// teardown did rather than only what it returned.
//
// THE LOCK AND THE DOORBELL ARE REAL: arch_kernel_lock excludes and services a pending
// doorbell while it spins, and sem_post takes IrqLock as kernel/sync/sync.cc does. An arm can
// therefore put a peer inside the dispatch entry with its post genuinely blocked on the lock a
// teardown holds, and a teardown that polls a peer's cell REFUSES here rather than hanging,
// because the peer answers every doorbell from inside the acquire loop.

#ifndef KICKOS_TESTS_UNIT_IRQQUIESCE_IRQ_SEAM_H
#define KICKOS_TESTS_UNIT_IRQQUIESCE_IRQ_SEAM_H

#include <stdint.h>

#include <atomic>

#include <kickos/instance.h>

namespace kickos
{
    namespace irqfix
    {
        enum SeamOp : uint8_t
        {
            OP_MASK = 0,
            OP_UNMASK = 1,
            OP_CLEAR = 2,
            OP_IPI_SEND = 3,
            OP_IPI_WAIT = 4
        };

        struct SeamEvent
        {
            SeamOp op;
            uint32_t arg; // a line for the controller calls, a core mask for the doorbell
        };

        // Which core arch_cpu_id answers, per thread: a threaded arm speaks as two cores at
        // once and klock keys its per-core row off this.
        extern thread_local uint32_t g_core;

        void reset();

        // The whole Kernel back to zero plus irq_init(), so an arm starts from a seeded
        // dispatch table and an empty binding pool.
        void reset_kernel();

        SeamEvent event(unsigned i);
        unsigned count_of(SeamOp op);
        // Index of the first event of that kind, or -1.
        int first_of(SeamOp op);
        // The last of OP_MASK / OP_UNMASK this line took, or -1 for neither: whether the line
        // was left armed, which is not answerable from a count.
        int last_line_op(int line);

        // A ONE-SHOT gate at arch_irq_mask, armed for one line. The next dispatch to mask that
        // line stops BEFORE the call is recorded, so it has already read the published pair and
        // holds no lock, which is the only state a rebind can beat a running handler from. An
        // unreleased gate gives up on a budget and raises mask_hold_timed_out(), so a missed
        // hand-off reddens rather than hanging.
        void hold_next_mask(int line);
        bool mask_hold_reached();
        bool mask_hold_timed_out();
        void release_mask_hold();

        // What the dispatch entry saw, recorded by the arms' own handlers.
        extern unsigned g_probe_calls;
        extern void* g_probe_arg;
        // Run inside a handler, once, on the core the handler is dispatched on.
        extern void (*g_probe_action)();

        // The stub capability layer: how many handles irq_claim's undo closed, and the object
        // handle the last cap_install was given.
        unsigned closes();
        int installed_handle();
        void reset_caps();

        // Doorbell pokes sent BY that core. Its own counter rather than a trace scan: the
        // threaded arms append to the trace from two threads and a count taken across that is
        // not one core's own tally.
        unsigned ipi_sends(uint32_t core);
        void bump_ipi_sends(uint32_t core);

        // --- what the threaded arms hand off on -------------------------------------------
        // Raised by arch_kernel_lock the first time a claim fails, so a core is spinning for a
        // word another core holds. An arm waits on this to know its peer is wedged inside the
        // dispatch entry rather than merely started.
        extern std::atomic<bool> g_lock_blocked;
        // How many times sem_post reached its body, which is past the lock it takes.
        extern std::atomic<unsigned> g_posts;

        // Clears the doorbell cells and the blocked flag between arms; every arm must leave
        // the lock word balanced itself.
        void reset_lock();
    }
}

#endif
