// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The scheduler core. reschedule() is the single point where a context switch
// is decided (invariant #2); every trigger funnels through it. The tick is not
// special. A pluggable policy (RTEMS-style) decides pick_next; FIFO+RR ship.

#ifndef KICKOS_SCHED_H
#define KICKOS_SCHED_H

#include <stdint.h>

#include <kickos/thread.h>

namespace kickos
{
    // Pluggable scheduling policy: the core owns mechanism (run state, context
    // switch, ready structure); the policy decides which thread runs next and how
    // slices behave. EDF / rate-monotonic can drop in later without touching
    // reschedule(), sync, or the arch layer.
    struct SchedPolicy
    {
        Thread* (*pick_next)();
        void (*on_ready)(Thread*);        // a thread became runnable
        void (*on_remove)(Thread*);       // a thread left the run set
        void (*on_yield)(Thread*);        // current voluntarily yielded
        void (*on_slice_expire)(Thread*); // RR quantum elapsed
    };

    namespace sched
    {
        void init();
        void set_policy(SchedPolicy const* policy);

        // Register a fully-initialized thread as READY.
        void add(Thread* t);

        // Enter the first thread from the boot context. Returns only on shutdown.
        void start();

        // The single decision point. Safe to call from thread or ISR context.
        void reschedule();

        // Voluntary yield: rotate within priority, then reschedule.
        void yield();

        // Remove `current` from the run set (state must already be set to the reason,
        // e.g. BLOCKED/SLEEPING), then reschedule. Returns when the thread is resumed.
        void block_current();

        // Remove `current` from the ready list WITHOUT rescheduling. A blocking
        // primitive must call this BEFORE parking the thread on a wait queue,
        // since the ready list and wait queues share the qnext/qprev links.
        void detach_current();

        // Make a previously-removed thread runnable again; preempts if warranted.
        void wake(Thread* t);

        // Terminate the current thread; never returns.
        void exit_current() __attribute__((noreturn));

        Thread* current();
        Thread* idle();

        // Live non-idle thread count (0 => nothing left to run).
        unsigned live_count();

        // RR support consumed by the time subsystem.
        // Absolute deadline (ns) at which the running thread's RR slice expires, or
        // UINT64_MAX when the running thread is not slice-limited.
        uint64_t next_slice_deadline();
        // Called from the timer ISR: if the running RR slice has elapsed at `now`,
        // rotate within priority and reschedule.
        void tick_rr(uint64_t now);
    }
}

#endif
