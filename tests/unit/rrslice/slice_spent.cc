// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The slice deadline is absolute, so a thread preempted across it has spent its slice. A resume
// that armed it a fresh quantum would let a thread preempted at every turn run on slivers
// forever while its equal-priority peer never gets the core.

#include <kickos/config/board.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_PAIR = 8;
    constexpr uint32_t QUANTUM_NS = 1000000u;

    Thread* rr(int slot)
    {
        Thread* const t = spawn(slot, PRIO_PAIR);
        t->policy = Policy::RR;
        t->quantum_ns = QUANTUM_NS;
        t->slice_deadline_ns = UINT64_MAX;
        return t;
    }
}

class RrSlice : public kickos::testfix::KSeam
{
};

TEST_F(RrSlice, a_first_switch_in_arms_a_whole_quantum)
{
    Thread* const a = rr(0);
    g_now_ns = 1000000u;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    EXPECT_EQ(a->slice_deadline_ns, g_now_ns + QUANTUM_NS);
}

TEST_F(RrSlice, a_resume_inside_the_slice_keeps_its_deadline)
{
    Thread* const a = rr(0);
    g_now_ns = 1000000u;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    uint64_t const armed = a->slice_deadline_ns;
    g_now_ns += QUANTUM_NS / 2u;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    EXPECT_EQ(a->slice_deadline_ns, armed);
}

TEST_F(RrSlice, a_resume_past_the_slice_expires_it_instead_of_refunding_it)
{
    Thread* const a = rr(0);
    g_now_ns = 1000000u;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    g_now_ns += 5u * QUANTUM_NS;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    EXPECT_LE(a->slice_deadline_ns, g_now_ns + KICKOS_TIMER_MIN_DELTA_NS);
    EXPECT_GT(a->slice_deadline_ns, g_now_ns);
}

TEST_F(RrSlice, a_fresh_thread_is_armed_a_whole_quantum)
{
    Thread* const a = spawn(0, PRIO_PAIR); // slice_deadline_ns left at its fresh zero
    a->policy = Policy::RR;
    a->quantum_ns = QUANTUM_NS;
    g_now_ns = 1000000u;
    {
        IrqLock lock;
        kernel().policy->on_switch_in(a);
    }
    EXPECT_EQ(a->slice_deadline_ns, g_now_ns + QUANTUM_NS);
}
