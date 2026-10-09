// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/time.h>
#include <kickos/bench.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/ktrace.h>

#include <kickos/sys/errno.h>

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/trace/record.h>
#endif

namespace kickos
{
    namespace
    {
        // Sorted ascending by deadline, singly-linked through tnext, rooted at the kernel.

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

    uint64_t ktime_now()
    {
        return arch_clock_now();
    }

    void ktime_rearm(Thread const* incoming, Held)
    {
#if KICKOS_BENCH
        BenchScope const bench_body(PH_KTIME_REARM);
#endif
        uint64_t next = UINT64_MAX;
        if (kernel().sleepq != nullptr)
        {
            next = kernel().sleepq->deadline_ns;
        }

        uint64_t event = sched::next_timed_event(incoming);
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

        // NO min-delta floor here, deliberately: this runs on EVERY context switch, and a
        // floor re-derived from the clock would change `next` on every call, which is the
        // quantity this dedup rests on. The floor belongs where the deadline is BORN, against
        // ONE clock reading (ktime_floored).
        uint64_t& armed = kernel().timer_armed_ns[kickos_kernel_core()];
        if (next == armed)
        {
            return;
        }
        armed = next;
        if (next == UINT64_MAX)
        {
            arch_timer_disarm();
            return;
        }
        arch_timer_arm(next);
    }

    // THE ONE PLACE A FIRED OR ABANDONED COMPARATOR IS FORGOTTEN, and the whole correctness of
    // the dedup above. A backend that clamps a far deadline into its counter fires early and
    // is re-armed for the SAME absolute deadline; skipping that re-arm starves the sleeper
    // for good. An LX6 goes further: its pending CCOMPARE0 match is cleared only by the next
    // write to that register, so a skipped re-arm leaves a raise standing.
    void ktime_disarm(Held)
    {
        kernel().timer_armed_ns[kickos_kernel_core()] = UINT64_MAX;
        arch_timer_disarm();
    }

    void ktime_sleep_until(uint64_t deadline_ns)
    {
        IrqLock lock;
        Thread* c = sched::current();
        // Ahead of every side effect below: sched::exit_current does not sweep the sleep
        // queue, so an exit after the insert would leave this thread on it.
        ParkToken const ask = park_cancel_pending(c);
        if (ask.cancelled())
        {
            sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN, &lock);
        }
        c->deadline_ns = ktime_floored(ktime_now(), deadline_ns);
        park_queueless(ask)(c, WAIT_SLEEP, nullptr, lock);
        sleepq_insert(c);
        sched::reschedule(nullptr, lock); // its switch arms the timer for the incoming thread
    }

    void ktime_deadline_arm(Thread* t, uint32_t timeout_us, Held)
    {
        if (timeout_us == KOS_TIMEOUT_NONE)
        {
            return;
        }
        uint64_t const now = ktime_now();
        t->deadline_ns = ktime_floored(now, now + static_cast<uint64_t>(timeout_us) * 1000u);
        sleepq_insert(t);
    }

    void ktime_deadline_cancel(Thread* t, Held)
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
        // sleep(0) yields instead of parking, and 0 < ns < min-delta still rounds UP to the
        // min slice: a delay promises time off-CPU, yield() returns at once with no peer.
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
        // BEFORE anything reads the queue: the comparator has fired, so what the kernel
        // recorded for it no longer describes the hardware.
        ktime_disarm(lock);
        uint64_t now = ktime_now();

        // MUST precede the wake loop: sched::wake reassigns kernel().current and tick_rr
        // reads it, so running it after drops any slice expiry landing on the same interrupt
        // as a sleeper wake.
        sched::tick_rr(now, lock);

        while (kernel().sleepq != nullptr and kernel().sleepq->deadline_ns <= now)
        {
            Thread* t = kernel().sleepq;
            sleepq_remove(t);
            // sleepq_remove leaves the wait edge: a timed wait is on the sleepq AND a wait
            // queue at once, and the unwind reads the edge to find the second.
            thread_abort_park(t, -KOS_ETIMEDOUT, lock);
        }

        ktime_rearm(sched::current(), lock);
    }

}

// Arch timer-expiry callback (tickless deadline or, if enabled, periodic tick).
extern "C" void kickos_isr_timer(void)
{
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
#if KICKOS_BENCH_SCHED_ON
    uint32_t const bench_was = ::kickos_bench_reason_enter(::kickos::BR_TIMER);
#endif
    ::kickos::ktime_on_timer();
#if KICKOS_BENCH_SCHED_ON
    ::kickos_bench_reason_leave(bench_was);
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
}
