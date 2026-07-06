// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tickless time. A delta list of absolute deadlines (sorted ascending) drives
// a single one-shot next-event timer armed for min(nearest sleeper, running-RR
// slice), with a minimum-delta guard so we never program a compare already in
// the past. Nothing time-pending => timer disarmed (zero timer interrupts).
// CONFIG_SCHED_PERIODIC_TICK forces a classic periodic tick instead.

#include <kickos/time.h>
#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/ktrace.h>
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/trace/record.h>
#endif

namespace kickos
{
    namespace
    {
        // Sorted (ascending deadline) singly-linked list of sleeping threads,
        // rooted at kernel().sleepq.

        void sleepq_insert(Thread* t)
        {
            Thread** pp = &kernel().sleepq;
            while (*pp != nullptr and (*pp)->deadline_ns <= t->deadline_ns)
            {
                pp = &(*pp)->tnext;
            }
            t->tnext = *pp;
            *pp = t;
            t->on_timer = true;
        }

        void sleepq_remove(Thread* t)
        {
            Thread** pp = &kernel().sleepq;
            while (*pp != nullptr and *pp != t)
            {
                pp = &(*pp)->tnext;
            }
            if (*pp == t)
            {
                *pp = t->tnext;
                t->tnext = nullptr;
                t->on_timer = false;
            }
        }
    }

    void ktime_init()
    {
        kernel().sleepq = nullptr;
    }

    uint64_t ktime_now()
    {
        return arch_clock_now();
    }

    void ktime_rearm()
    {
        IrqLock lock;
        uint64_t next = UINT64_MAX;
        if (kernel().sleepq != nullptr)
        {
            next = kernel().sleepq->deadline_ns;
        }

        uint64_t event = sched::next_timed_event();
        if (event < next)
        {
            next = event;
        }

#if defined(KICKOS_SCHED_PERIODIC_TICK)
        uint64_t periodic = ktime_now() + KICKOS_TICK_PERIOD_NS;
        if (periodic < next)
        {
            next = periodic;
        }
#endif

        if (next == UINT64_MAX)
        {
            arch_timer_disarm(); // tickless: nothing pending
            return;
        }

        // Minimum-delta guard: never arm a compare that may already be in the past.
        uint64_t now = ktime_now();
        uint64_t floor = now + KICKOS_TIMER_MIN_DELTA_NS;
        if (next < floor)
        {
            next = floor;
        }
        arch_timer_arm(next);
    }

    void ktime_sleep_until(uint64_t deadline_ns)
    {
        IrqLock lock;
        Thread* c = sched::current();
        c->deadline_ns = deadline_ns;
        c->state = ThreadState::SLEEPING;
        sleepq_insert(c);
        ktime_rearm();
        sched::block_current(); // drops from run set + reschedules; returns on wake
    }

    void ktime_sleep_ns(uint64_t ns)
    {
        // sleep(0) == yield: relinquish and return, do NOT park. Deliberately NOT
        // extended to 0 < ns < min-delta -- those still round UP to the min slice:
        // a delay promises time off-CPU, whereas yield() returns at once with no peer.
        if (ns == 0)
        {
            sched::yield();
            return;
        }
        // Saturate on overflow: a caller-supplied huge ns must not wrap to a past
        // deadline (which the min-delta guard would turn into a ~20us sleep).
        uint64_t now = ktime_now();
        uint64_t deadline = now + ns;
        if (deadline < now)
        {
            deadline = UINT64_MAX;
        }
        ktime_sleep_until(deadline);
    }

    void ktime_on_timer()
    {
        IrqLock lock;
        uint64_t now = ktime_now();

        // Wake every sleeper whose deadline has passed.
        while (kernel().sleepq != nullptr and kernel().sleepq->deadline_ns <= now)
        {
            Thread* t = kernel().sleepq;
            sleepq_remove(t);
            sched::wake(t); // readies + reschedules (deferred in ISR ctx)
        }

        // Round-robin slice accounting for the running thread.
        sched::tick_rr(now);

        ktime_rearm();
    }

}

// Arch timer-expiry callback (tickless deadline or, if enabled, periodic tick).
extern "C" void kickos_isr_timer(void)
{
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
    ::kickos::ktime_on_timer();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
}
