// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_ASSERT_EXCLUSION_HELD, the check a "caller holds the exclusion" body makes of its
// caller (kickos/sched.h), AT TWO KERNEL CORES: at one core the macro is nothing at all and
// none of these arms is expressible there.
//
// The third arm is why the check reads `owed` and not the depth alone. A swap that is merely
// BOOKED leaves the depth off the core with the lock still held, and the bracket that booked
// it then unwinds to zero: a body reached before the exception epilogue parks the frame is
// inside the exclusion while the depth says nothing.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO = 5;

    Thread* blocked(int slot)
    {
        Thread* t = spawn(slot, PRIO);
        kernel().policy->on_remove(t);
        t->state = ThreadState::BLOCKED;
        return t;
    }
}

class HeldAssert : public kickos::testfix::KSeam
{
};

TEST_F(HeldAssert, a_held_body_reached_outside_the_bracket_dies)
{
    Thread* t = blocked(0);

    KICKOS_EXPECT_PANIC(sched::wake(t), "debug assert: ::kickos::klock_exclusion_held");
}

TEST_F(HeldAssert, the_same_call_under_a_bracket_stands)
{
    Thread* t = blocked(0);

    {
        IrqLock lock;
        sched::wake(t);
    }

    EXPECT_NE(t->state, ThreadState::BLOCKED)
        << "the wake did not ready the thread, so the arm above proves nothing about the "
           "bracket: it would die on any call at all";
}

TEST_F(HeldAssert, a_booked_swap_leaves_the_exclusion_held_at_depth_zero)
{
    Thread* t = blocked(0);

    {
        IrqLock lock;
        // What switch_to does around a swap the backend only books.
        uint32_t const depth = klock_detach();
        klock_attach(depth);
    }

    sched::set_prio(t, PRIO + 1);
    EXPECT_EQ(t->prio, PRIO + 1);

    // The exception epilogue's park, which is what ends the span.
    kickos_switch_unlock();
}
