// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test KICKOS_ASSERT_EXCLUSION_HELD on two cores. Single-core builds omit it.
// A deferred switch may clear depth while the lock remains held; the check
// must include the pending release state.

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
        // Model a deferred switch.
        uint32_t const depth = klock_detach();
        klock_attach(depth);
    }

    sched::set_prio(t, PRIO + 1);
    EXPECT_EQ(t->prio, PRIO + 1);

    // Model lock release by the exception epilogue.
    kickos_switch_unlock();
}
