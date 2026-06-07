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
    // Pluggable scheduling policy (RTEMS-style). The core is pure mechanism (run
    // state, current, the context switch); the policy owns WHICH thread runs next
    // -- it holds the ready structure and enqueues/dequeues through these hooks,
    // the core never touches ready state. EDF / rate-monotonic drop in here.
    struct SchedPolicy
    {
        // Scheduling decision.
        Thread* (*pick_next)();           // highest-priority runnable thread
        void (*on_ready)(Thread*);        // enqueue a now-runnable thread
        void (*on_remove)(Thread*);       // dequeue a thread leaving the run set
        void (*on_yield)(Thread*);        // current voluntarily yielded
        void (*on_slice_expire)(Thread*); // the running thread's timed slice elapsed

        // Timed-event seam (RR today): the core owns the clock, the policy the
        // deadline. on_switch_in arms the incoming thread; next_timed_event is the
        // earliest policy deadline for the tickless timer (UINT64_MAX = none).
        void (*on_switch_in)(Thread*);
        uint64_t (*next_timed_event)();
    };

    namespace sched
    {
        void init();
        void set_policy(SchedPolicy const* policy);

        // The built-in FIFO + round-robin policy (kernel/sched/policy_fifo_rr.cc);
        // installed by init(). Other policies swap in via set_policy().
        SchedPolicy const* default_policy();

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
        // since the ready list and wait queues share the TCB link node.
        void detach_current();

        // Make a previously-removed thread runnable again; preempts if warranted.
        void wake(Thread* t);

        // Terminate the current thread; never returns.
        void exit_current() __attribute__((noreturn));

        Thread* current();
        Thread* idle();

        // Live non-idle thread count (0 => nothing left to run).
        unsigned live_count();

        // The active policy's earliest timed event (ns), or UINT64_MAX for none.
        // Consumed by the time subsystem when arming the tickless timer (RR slice
        // expiry today; the core carries no notion of a "slice").
        uint64_t next_timed_event();
        // Called from the timer ISR on every expiry: if the active policy has a
        // timed event due at `now` (an RR slice), let it act, then reschedule.
        void tick_rr(uint64_t now);
    }
}

#endif
