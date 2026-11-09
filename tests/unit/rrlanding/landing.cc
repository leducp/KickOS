// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two round-robin threads of one priority, with a quantum under the min delta, rotated by a timer
// handler and a switch that each cost several min deltas. Wherever the comparator is programmed
// for a deadline the work before the thread's first instruction has already used up, the expiry
// is pending at the return, every rotation installs a thread that is rotated out before it runs,
// and neither thread ever runs again. Each thread rotated in must instead run at least the min
// delta before its next expiry.

#include <kickos/arch/arch.h>
#include <kickos/config/board.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_PAIR = 8;
    constexpr uint32_t QUANTUM_NS = 1000u;
    constexpr uint64_t MIN_NS = KICKOS_TIMER_MIN_DELTA_NS;
    constexpr uint64_t SLOW_NS = 3u * MIN_NS;
    constexpr int TURNS = 40;
    // A storm takes the timer again before every landing; this many in a row ends the arm.
    constexpr int STORM_BOUND = 1000;

    Thread* g_pair[2];
    uint64_t g_ran[2];
    int g_storm;
    uint32_t g_switches_landed;

    Thread* rr(int slot)
    {
        Thread* const t = spawn(slot, PRIO_PAIR);
        t->policy = Policy::RR;
        t->quantum_ns = QUANTUM_NS;
        t->slice_deadline_ns = UINT64_MAX;
        return t;
    }

    Thread* running()
    {
        return kernel().current(kickos_kernel_core());
    }

    // The thread the core returns into runs until the comparator fires.
    void run_until_timer()
    {
        if (g_timer_due <= g_now_ns)
        {
            return;
        }
        Thread* const c = running();
        for (int i = 0; i < 2; i++)
        {
            if (c == g_pair[i])
            {
                g_ran[i] += g_timer_due - g_now_ns;
            }
        }
        g_now_ns = g_timer_due;
    }

#if KICKOS_ARCH_SWITCH_LANDS_LATER
    // The handler's cost falls before its booking, in the seating. A booked switch lands in a
    // switcher exception of its own that costs `switch_cost` and that the timer preempts
    // whenever the comparator comes due first.
    void timer_interrupt()
    {
        g_in_isr = true;
        kickos_isr_timer();
        g_in_isr = false;
    }

    void land(uint64_t switch_cost)
    {
        while (g_switches != g_switches_landed and g_storm < STORM_BOUND)
        {
            uint64_t const end = g_now_ns + switch_cost;
            if (g_timer_due <= end)
            {
                if (g_timer_due > g_now_ns)
                {
                    g_now_ns = g_timer_due;
                }
                g_storm++;
                timer_interrupt();
                continue;
            }
            g_now_ns = end;
            g_switches_landed = g_switches;
            kickos_switch_landed(&running()->ctx);
        }
    }
#else
    // The switch lands at the handler's own exit, behind SLOW_NS of handler work done after the
    // booking.
    void timer_interrupt()
    {
        g_in_isr = true;
        ktime_on_timer();
        g_now_ns += SLOW_NS;
        ktime_land_at_isr_exit();
        g_in_isr = false;
    }

    void land(uint64_t)
    {
    }
#endif
}

class RrLanding : public KSeam
{
};

TEST_F(RrLanding, every_rotated_in_thread_runs_the_min_delta_before_its_expiry)
{
    g_pair[0] = rr(0);
    g_pair[1] = rr(1);
    g_ran[0] = 0;
    g_ran[1] = 0;
    g_storm = 0;
    g_switches_landed = g_switches;
    g_now_ns = 1000000u;
    {
        IrqLock lock;
        sched::reschedule(nullptr, lock);
    }
    land(0u);
    g_seat_cost_ns = SLOW_NS;

    for (int turn = 0; turn < TURNS; turn++)
    {
        ASSERT_NE(g_timer_due, UINT64_MAX) << "turn " << turn << ": no expiry armed";
        uint64_t const before[2] = {g_ran[0], g_ran[1]};
        Thread* const runner = running();
        run_until_timer();
        uint64_t const got = (g_ran[0] - before[0]) + (g_ran[1] - before[1]);
        ASSERT_GE(got, MIN_NS) << "turn " << turn << ": thread " << runner->name
                               << " was rotated in and ran " << got << " ns before its expiry";
        timer_interrupt();
        land(SLOW_NS);
        ASSERT_LT(g_storm, STORM_BOUND)
            << "the timer was taken " << g_storm << " times without a switch landing";
    }
    EXPECT_GE(g_ran[0], (TURNS / 2 - 1) * MIN_NS);
    EXPECT_GE(g_ran[1], (TURNS / 2 - 1) * MIN_NS);
}

#if KICKOS_ARCH_SWITCH_LANDS_LATER
namespace
{
    // A thread booked with `deadline` and not landed yet takes the timer with its peer ready: it
    // must still be the one the core returns into, and its landing must grant it a live slice.
    void booked_thread_takes_the_timer(uint64_t deadline)
    {
        g_pair[0] = rr(0);
        g_pair[1] = rr(1);
        g_pair[0]->slice_deadline_ns = deadline;
        g_pair[1]->slice_deadline_ns = deadline;
        g_now_ns = 1000000u;
        uint32_t const before = g_switches;
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        Thread* const booked = running();
        ASSERT_NE(g_switches, before) << "no switch was booked";
        ASSERT_NE(kernel().switch_landing_owed[kickos_kernel_core()], 0u);
        uint32_t const booked_at = g_switches;
        g_now_ns += SLOW_NS;
        timer_interrupt();
        EXPECT_EQ(running(), booked) << "rotated out before its switch landed";
        EXPECT_EQ(g_switches, booked_at);
        kickos_switch_landed(&booked->ctx);
        EXPECT_GT(booked->slice_deadline_ns, g_now_ns);
        EXPECT_GT(g_timer_due, g_now_ns);
    }
}

TEST_F(RrLanding, a_timer_before_the_landing_takes_no_expiry_against_a_fresh_booked_thread)
{
    booked_thread_takes_the_timer(0u);
}

TEST_F(RrLanding, a_timer_before_the_landing_takes_no_expiry_against_a_stale_booked_thread)
{
    booked_thread_takes_the_timer(1000u);
}

namespace
{
    // A switch to `t` has no slice to stamp, so its landing writes no comparator and masks
    // nothing: the booking armed what it needs.
    void lands_for_free(Thread* t)
    {
        g_now_ns = 1000000u;
        uint32_t const before = g_switches;
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_NE(g_switches, before) << "no switch was booked";
        ASSERT_EQ(running(), t);
        EXPECT_EQ(kernel().switch_landing_owed[kickos_kernel_core()], 0u)
            << "the booking left a landing owed";
        uint64_t const due = g_timer_due;
        uint32_t const writes = g_timer_writes;
        uint32_t const saves = g_irq_saves;
        g_now_ns += SLOW_NS;
        kickos_switch_landed(&t->ctx);
        EXPECT_EQ(g_timer_writes, writes) << "the landing wrote the comparator";
        EXPECT_EQ(g_irq_saves, saves) << "the landing took the lock";
        EXPECT_EQ(g_timer_due, due);
    }
}

TEST_F(RrLanding, a_fifo_switch_lands_with_no_lock_and_no_comparator_write)
{
    lands_for_free(spawn(0, PRIO_PAIR));
}

TEST_F(RrLanding, a_live_slice_lands_with_no_lock_and_no_comparator_write)
{
    Thread* const t = rr(0);
    t->slice_deadline_ns = 1000000u + 10u * MIN_NS;
    lands_for_free(t);
    EXPECT_EQ(t->slice_deadline_ns, 1000000u + 10u * MIN_NS) << "the live slice was refunded";
    EXPECT_EQ(g_timer_due, t->slice_deadline_ns) << "the booking did not arm the live slice";
}
#endif
