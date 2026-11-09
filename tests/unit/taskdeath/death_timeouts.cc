// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Every death waiter on one task timing out in a single timer interrupt: the task's threads end
// with no lend of a waiter gone, and the current that lend kept running gives way to the highest
// waiter it woke.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class DeathTimeouts : public KSeam
            {
            };

            constexpr uint8_t MEMBER_PRIO = 2;
            constexpr uint64_t WAIT_NS = 1000000u;

            Thread* member(int slot, Task* t)
            {
                Thread* const m = seat_pool(slot, MEMBER_PRIO);
                attach_caps(m, KICKOS_SEAM_CAP_WIDTH);
                join_task(m, t);
                return m;
            }
        }

        TEST_F(DeathTimeouts, waiters_timing_out_together_leave_no_lend_and_the_highest_runs)
        {
            Task* const t = task(1);
            Thread* const m1 = member(1, t);
            Thread* const m2 = member(2, t);
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), m1) << "fixture: the first member does not run";
            uint8_t const prios[] = {9, 8, 7};
            Thread* w[3] = {};
            uint64_t const at = g_now_ns + WAIT_NS;
            {
                IrqLock lock;
                for (int i = 0; i < 3; i++)
                {
                    w[i] = seat_pool(3 + i, prios[i]);
                    park_death_waiter(w[i], t);
                    ktime_deadline_arm_at(w[i], at, false, lock);
                }
                death_wait_lend(t, lock);
            }
            ASSERT_EQ(m1->prio, 9) << "fixture: the waiters lend the members nothing";
            ASSERT_EQ(m2->prio, 9);
            g_now_ns = at;
            ktime_on_timer();
            for (Thread* const each : w)
            {
                EXPECT_EQ(each->wait_kind, WAIT_NONE);
                EXPECT_EQ(each->wait_result, -KOS_ETIMEDOUT);
            }
            EXPECT_TRUE(kernel().death_waiters.empty());
            EXPECT_TRUE(kernel().death_dirty.empty());
            EXPECT_EQ(m1->prio, MEMBER_PRIO) << "a member keeps the lend of a waiter gone";
            EXPECT_EQ(m2->prio, MEMBER_PRIO);
            EXPECT_EQ(sched::current(), w[0])
                << "the member the lend held on the core kept it past the highest waiter";
        }
    }
}
