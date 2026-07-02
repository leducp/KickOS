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
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

namespace kickos
{
    namespace
    {

        // Sorted (ascending deadline) singly-linked list of sleeping threads.
        Thread* g_sleepq = nullptr;

#if defined(KICKOS_SCHED_PERIODIC_TICK)
        constexpr uint64_t kTickPeriodNs = 1000000ull; // 1 ms periodic tick
#endif

        void sleepq_insert(Thread* t)
        {
            Thread** pp = &g_sleepq;
            while (*pp != nullptr && (*pp)->deadline_ns <= t->deadline_ns)
            {
                pp = &(*pp)->tnext;
            }
            t->tnext = *pp;
            *pp = t;
            t->on_timer = true;
        }

        void sleepq_remove(Thread* t)
        {
            Thread** pp = &g_sleepq;
            while (*pp != nullptr && *pp != t) pp = &(*pp)->tnext;
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
        g_sleepq = nullptr;
    }

    uint64_t ktime_now()
    {
        return arch_clock_now();
    }

    void ktime_rearm()
    {
        IrqLock lock;
        uint64_t next = UINT64_MAX;
        if (g_sleepq != nullptr) next = g_sleepq->deadline_ns;

        uint64_t slice = sched::next_slice_deadline();
        if (slice < next) next = slice;

#if defined(KICKOS_SCHED_PERIODIC_TICK)
        uint64_t periodic = ktime_now() + kTickPeriodNs;
        if (periodic < next) next = periodic;
#endif

        if (next == UINT64_MAX)
        {
            arch_timer_disarm(); // tickless: nothing pending
            return;
        }

        // Minimum-delta guard: never arm a compare that may already be in the past.
        uint64_t now = ktime_now();
        uint64_t floor = now + KICKOS_TIMER_MIN_DELTA_NS;
        if (next < floor) next = floor;
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
        ktime_sleep_until(ktime_now() + ns);
    }

    void ktime_on_timer()
    {
        IrqLock lock;
        uint64_t now = ktime_now();

        // Wake every sleeper whose deadline has passed.
        while (g_sleepq != nullptr && g_sleepq->deadline_ns <= now)
        {
            Thread* t = g_sleepq;
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
    ::kickos::ktime_on_timer();
}
