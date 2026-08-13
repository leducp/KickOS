// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Tickless time. A delta list of absolute deadlines (sorted ascending) drives
// a single one-shot next-event timer armed for min(nearest sleeper, running-RR
// slice), with a minimum-delta guard so we never program a compare already in
// the past. Nothing time-pending => timer disarmed (zero timer interrupts).
// CONFIG_SCHED_PERIODIC_TICK forces a classic periodic tick instead.

#include <kickos/time.h>
#include <kickos/endpoint.h> // endpoint_wait_abort: park.cc owns every EP unwind
#include <kickos/sched.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/ktrace.h>

#include <kickos/sys/errno.h> // KOS_ETIMEDOUT: the value every expiry arm delivers

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/trace/record.h>
#endif

namespace kickos
{
    namespace
    {
        // Sorted ascending by deadline, singly-linked through tnext, rooted at
        // kernel().sleepq.

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
            arch_timer_disarm();
            return;
        }

        // NO min-delta floor here, deliberately. This runs on EVERY context switch, and a
        // floor re-derived from the clock would make `next` a different value on every
        // call, which is the quantity the backends dedup their arm on. The dedup would
        // never hit, the one-shot would restart from zero before reaching its compare, and
        // a deadline inside the min-delta window would starve for as long as switches keep
        // arriving. The floor belongs where the deadline is BORN, against ONE clock
        // reading: ktime_sleep_until below for a sleeper, arm_slice for an RR quantum.
        // An already-past deadline is an immediate fire; each backend floors its programmed
        // delta at one tick to get that.
        arch_timer_arm(next);
    }

    void ktime_sleep_until(uint64_t deadline_ns)
    {
        IrqLock lock;
        Thread* c = sched::current();
        // The floor is applied HERE, once, against one clock reading. See ktime_rearm.
        uint64_t const floor = ktime_now() + KICKOS_TIMER_MIN_DELTA_NS;
        if (deadline_ns < floor)
        {
            deadline_ns = floor;
        }
        c->deadline_ns = deadline_ns;
        c->state = ThreadState::BLOCKED;
        // All three fields, like every other park: a sleeper is on no wait queue, and
        // leaving the field unwritten would make this park correct only by virtue of
        // whoever cleared it last.
        c->wait_queue = nullptr;
        c->wait_kind = WAIT_SLEEP;
        c->wait_obj = nullptr; // the delta list is rooted in the Kernel, not in an object
        sleepq_insert(c);
        ktime_rearm();
        sched::block_current(); // returns on wake
    }

    void ktime_deadline_arm(Thread* t, uint32_t timeout_us)
    {
        // The floor is applied HERE, once, against one clock reading. See ktime_rearm.
        uint64_t const now = ktime_now();
        uint64_t deadline = now + static_cast<uint64_t>(timeout_us) * 1000u;
        uint64_t const floor = now + KICKOS_TIMER_MIN_DELTA_NS;
        if (deadline < floor)
        {
            deadline = floor;
        }
        t->deadline_ns = deadline;
        sleepq_insert(t);
    }

    void ktime_deadline_cancel(Thread* t)
    {
        if (not t->on_timer)
        {
            return;
        }
        sleepq_remove(t);
        // No ktime_rearm: switch_to rearms on every context switch, and a timer left armed
        // early only costs a wakeup that finds nothing expired.
    }

    void ktime_sleep_ns(uint64_t ns)
    {
        // sleep(0) yields instead of parking. Deliberately NOT extended to
        // 0 < ns < min-delta, which still rounds UP to the min slice: a delay promises time
        // off-CPU, whereas yield() returns at once when there is no peer.
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

        // MUST precede the wake loop: sched::wake reassigns kernel().current and tick_rr
        // reads it, so running it after silently drops any slice expiry landing on the same
        // interrupt as a sleeper wake. On a coarse clock the two deadlines quantise
        // together, so that is the common case, not a corner.
        sched::tick_rr(now);

        while (kernel().sleepq != nullptr and kernel().sleepq->deadline_ns <= now)
        {
            Thread* t = kernel().sleepq;
            sleepq_remove(t);
            // The tag is cleared HERE and never in sleepq_remove: a timed wait is on the
            // sleepq AND a wait queue at once, so a clear there would erase the very edge
            // this dispatch has to read. sched::wake reuses `link` for the ready list, so a
            // thread still linked on a wait queue must leave it BEFORE the wake.
            switch (t->wait_kind)
            {
                case WAIT_SLEEP:
                {
                    t->clear_wait_edge();
                    sched::wake(t);
                    break;
                }
                case WAIT_JOIN:
                {
                    // On no list at all, so clearing the tag IS the whole unwind. It also
                    // makes exit_current's sweep miss a joiner that has already given up.
                    t->clear_wait_edge();
                    t->wait_result = -KOS_ETIMEDOUT;
                    sched::wake(t);
                    break;
                }
                case WAIT_EP_SEND:
                case WAIT_EP_RECV:
                case WAIT_EP_REPLY:
                {
                    // Delegated whole: which list to unlink from and which priority
                    // donation to revert are endpoint internals, and this file must not
                    // learn them. See endpoint_wait_abort (kickos/endpoint.h).
                    endpoint_wait_abort(t, -KOS_ETIMEDOUT);
                    break;
                }
                default:
                {
                    kpanic(diag::kDeadlineNoTimer);
                }
            }
        }

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
