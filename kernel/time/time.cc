// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/time.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>
#include <kickos/kernel.h>
#include <kickos/ktrace.h>

#include <kickos/sys/errno.h>
#include <kickos/sys/witness.h>

#if KICKOS_WITNESS_UACCESS
namespace kickos
{
    // syscall_witness.cc: a ring-0 read the witness armed for the thread this interrupt took,
    // and the deadline it is due at, UINT64_MAX for any other thread.
    void witness_uaccess_timer(Thread const* c);
    uint64_t witness_uaccess_due(Thread const* t);
}
#endif

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/trace/record.h>
#endif

namespace kickos
{
    namespace
    {
        // From `*pp` on, which no later deadline precedes; answers the link after `t`.
        Thread** sleepq_insert_from(Thread** pp, Thread* t)
        {
            while (*pp != nullptr and (*pp)->deadline_ns <= t->deadline_ns)
            {
                pp = &(*pp)->tnext;
            }
            t->tnext = *pp;
            *pp = t;
            t->on_timer = true;
            return &t->tnext;
        }

        // Onto the calling core's own queue: the park's next switch on this core is what arms
        // the comparator for it.
        void sleepq_insert(Thread* t, bool wall)
        {
            KICKOS_DEBUG_ASSERT(queue_core_of(t) == kickos_kernel_core());
            (void)sleepq_insert_from(&kernel().sleepq(queue_core_of(t)), t);
            t->timer_wall = wall;
        }

        // `d` moved by `by`, held inside [0, UINT64_MAX].
        uint64_t shifted(uint64_t d, int64_t by)
        {
            if (by < 0)
            {
                uint64_t const back = static_cast<uint64_t>(-(by + 1)) + 1u;
                if (back >= d)
                {
                    return 0;
                }
                return d - back;
            }
            uint64_t const ahead = static_cast<uint64_t>(by);
            if (ahead > UINT64_MAX - d)
            {
                return UINT64_MAX;
            }
            return d + ahead;
        }

        // Takes the marked deadlines off core c's queue in its order, each moved by `by_ns` and
        // floored at `now`, which keeps that order. Answers them as a list, null for none.
        Thread* wall_take(KernelCore c, int64_t by_ns, uint64_t now)
        {
            Thread* moved = nullptr;
            Thread** tail = &moved;
            Thread** pp = &kernel().sleepq(c);
            while (*pp != nullptr)
            {
                Thread* const t = *pp;
                if (not t->timer_wall)
                {
                    pp = &t->tnext;
                    continue;
                }
                *pp = t->tnext;
                t->deadline_ns = ktime_floored(now, shifted(t->deadline_ns, by_ns));
                t->tnext = nullptr;
                *tail = t;
                tail = &t->tnext;
            }
            return moved;
        }

        // Puts wall_take's list back on core c's queue, in one pass.
        void wall_put_back(KernelCore c, Thread* moved)
        {
            Thread** pp = &kernel().sleepq(c);
            while (moved != nullptr)
            {
                Thread* const t = moved;
                moved = t->tnext;
                pp = sleepq_insert_from(pp, t);
            }
        }

        // From any core, keyed by queue_core, which must not move while a deadline is queued.
        void sleepq_remove(Thread* t)
        {
            Thread** pp = &kernel().sleepq(queue_core_of(t));
            while (*pp != nullptr and *pp != t)
            {
                pp = &(*pp)->tnext;
            }
            KICKOS_DEBUG_ASSERT(*pp == t);
            if (*pp == t)
            {
                *pp = t->tnext;
                t->tnext = nullptr;
                t->on_timer = false;
                t->timer_wall = false;
            }
        }
    }

    uint64_t ktime_now()
    {
        return arch_clock_now();
    }

    namespace
    {
        // The soonest event the core must wake for once `incoming` runs: the sleep queue's
        // head, or its policy deadline. A booked thread whose switch lands later contributes no
        // slice until ktime_land stamps it; sched::tick_rr skips it on the same flag.
        uint64_t next_event(Thread const* incoming)
        {
            uint64_t next = UINT64_MAX;
            Thread const* const head = kernel().sleepq(kickos_kernel_core());
            if (head != nullptr)
            {
                next = head->deadline_ns;
            }
#if KICKOS_ARCH_SWITCH_LANDS_LATER
            if (kernel().switch_landing_owed[kickos_kernel_core()] != 0)
            {
                incoming = nullptr;
            }
#endif
            uint64_t const event = sched::next_timed_event(incoming);
            if (event < next)
            {
                next = event;
            }
#if KICKOS_WITNESS_UACCESS
            uint64_t const witness = witness_uaccess_due(incoming);
            if (witness < next)
            {
                next = witness;
            }
#endif
#if KICKOS_SCHED_PERIODIC_TICK
            uint64_t const periodic = ktime_now() + KICKOS_TICK_PERIOD_NS;
            if (periodic < next)
            {
                next = periodic;
            }
#endif
            return next;
        }

        // Floored against the clock read here: the work between a deadline's birth and this
        // call can outlast the min delta, and a comparator programmed in the past raises again
        // before its handler returns, so the handler runs back to back and no thread is ever
        // resumed.
        void program(uint64_t next)
        {
            kernel().timer_armed_ns[kickos_kernel_core()] = next;
            if (next == UINT64_MAX)
            {
                arch_timer_disarm();
                return;
            }
            arch_timer_arm(ktime_floored(ktime_now(), next));
        }
    }

    void ktime_rearm(Thread const* incoming, Held)
    {
        uint64_t const next = next_event(incoming);
        // The dedup key is `next` itself, never a value re-derived from the clock: this runs on
        // every context switch, and a moving key would restart the countdown each time.
        if (next == kernel().timer_armed_ns[kickos_kernel_core()])
        {
            return;
        }
        program(next);
    }

    // Stamped from the clock read here, so the landed thread runs at least the min delta whatever
    // the switch cost before this point. Armed through the dedup: a reprogram on every landing
    // holds off a deadline inside the min-delta window for as long as landings keep coming.
    void ktime_land(Held held)
    {
        KernelCore const me = kickos_kernel_core();
        if (kernel().switch_landing_owed[me] == 0)
        {
            return;
        }
        kernel().switch_landing_owed[me] = 0;
        Thread* const t = sched::current();
        policy_on_switch_in(t);
        ktime_rearm(t, held);
    }

    void ktime_land_at_isr_exit()
    {
#if not KICKOS_ARCH_SWITCH_LANDS_LATER
        // Read unlocked: only this core writes it, and ktime_land tests it again under the lock.
        if (kernel().switch_landing_owed[kickos_kernel_core()] == 0)
        {
            return;
        }
        IrqLock lock;
        ktime_land(lock);
#endif
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
        sleepq_insert(c, false);
        sched::reschedule(nullptr, lock); // its switch arms the timer for the incoming thread
    }

    void ktime_deadline_arm(Thread* t, uint32_t timeout_us, Held held)
    {
        if (timeout_us == KOS_TIMEOUT_NONE)
        {
            return;
        }
        uint64_t const now = ktime_now();
        uint64_t const deadline = now + static_cast<uint64_t>(timeout_us) * 1000u;
        ktime_deadline_arm_at(t, ktime_floored(now, deadline), false, held);
    }

    void ktime_deadline_arm_at(Thread* t, uint64_t deadline_ns, bool wall, Held)
    {
        t->deadline_ns = deadline_ns;
        sleepq_insert(t, wall);
    }

    bool ktime_limit_passed(WaitLimit limit)
    {
        if (limit.on_wall())
        {
            return limit.unix_ns() <= realtime_now();
        }
        return limit.lo == 0u;
    }

    void ktime_limit_arm(Thread* t, WaitLimit limit, Held held)
    {
        if (limit.on_wall())
        {
            realtime_deadline_arm(t, limit.unix_ns(), held);
            return;
        }
        ktime_deadline_arm(t, limit.lo, held);
    }

    void ktime_wall_shift(int64_t by_ns, Held held)
    {
        if (by_ns == 0)
        {
            return;
        }
        uint64_t const now = ktime_now();
        KernelCore const me = kickos_kernel_core();
        uint32_t peers = 0;
        for (KernelCore const c : KernelCores::all())
        {
            if (c == me)
            {
                continue;
            }
            Thread* const moved = wall_take(c, by_ns, now);
            if (moved != nullptr)
            {
                wall_put_back(c, moved);
                peers = peers | (1u << c);
            }
        }
        // A deadline moved earlier on a peer's queue waits for that peer's comparator, which
        // only its own dispatch re-arms.
        sched_ask_at_span_end(peers);
        Thread* const moved = wall_take(me, by_ns, now);
        if (moved == nullptr)
        {
            return;
        }
        wall_put_back(me, moved);
        ktime_rearm(sched::current(), held);
    }

    void ktime_deadline_cancel(Thread* t, Held)
    {
        if (not t->on_timer)
        {
            return;
        }
        // No re-arm, even of a peer whose head this was: its comparator fires once for nothing
        // and re-arms from its own queue.
        sleepq_remove(t);
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

        death_leave_batch_begin();
        while (true)
        {
            Thread* const t = kernel().sleepq(kickos_kernel_core());
            if (t == nullptr or t->deadline_ns > now)
            {
                break;
            }
            sleepq_remove(t);
            // sleepq_remove leaves the wait edge: a timed wait is on the sleepq AND a wait
            // queue at once, and the unwind reads the edge to find the second.
            thread_abort_park(t, -KOS_ETIMEDOUT, lock);
        }
        death_leave_batch_end(lock);

        ktime_rearm(sched::current(), lock);
    }

}

// Arch timer-expiry callback (tickless deadline or, if enabled, periodic tick).
extern "C" void kickos_isr_timer(void)
{
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_enter(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
#if KICKOS_WITNESS_UACCESS
    ::kickos::witness_uaccess_timer(::kickos::sched::current());
#endif
    ::kickos::ktime_on_timer();
    ::kickos::ktime_land_at_isr_exit();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    ::kickos::ktrace_irq_exit(static_cast<uint16_t>(::kickos::trace::TRACE_TIMER_LINE));
#endif
}

#if KICKOS_ARCH_SWITCH_LANDS_LATER
static_assert(KICKOS_KERNEL_CORES == 1,
              "kickos_switch_landed takes no lock, its callers' mask being the exclusion: above "
              "one kernel core it needs the cross-core one");
#endif

namespace kickos
{
    // The landing hook of a KICKOS_ARCH_SWITCH_LANDS_LATER arch, called masked. A newer booking
    // taken before this one landed owns the flag, and that booking's own landing settles it.
    extern "C" void kickos_switch_landed(struct arch_context const* landed)
    {
        if (kernel().switch_landing_owed[kickos_kernel_core()] == 0)
        {
            return;
        }
        Thread const* const t = sched::current();
        if (t == nullptr or &t->ctx != landed)
        {
            return;
        }
        ktime_land(Held());
    }
}
