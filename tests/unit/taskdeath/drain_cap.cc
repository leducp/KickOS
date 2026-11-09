// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A lend whose drain reaches its pop bound leaves every death waiter linked once and lends no
// further, at a posture that lowers the bound to one pop so a two-hop chain reaches it.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            static_assert(KICKOS_PI_DRAIN_POPS == 1, "the posture lowers the drain to one pop");

            class DrainCap : public KSeam
            {
            };

            int linked(HeadList const& l)
            {
                int n = 0;
                for (ListNode* p = l.head; p != nullptr; p = p->next)
                {
                    n++;
                }
                return n;
            }

            Thread* member(int slot, uint8_t prio, Task* t)
            {
                Thread* const m = seat_pool(slot, prio);
                attach_caps(m, KICKOS_SEAM_CAP_WIDTH);
                join_task(m, t);
                return m;
            }
        }

        // t1's thread waits on a, a on t2's death, t2's thread on b, b on t3's death: the
        // second hop is past the bound.
        TEST_F(DrainCap, a_drain_past_its_bound_keeps_every_waiter_linked_and_lends_no_further)
        {
            Thread* const c = seat_pool(0, 9);
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), c);
            static uint32_t wa = 0;
            static uint32_t wb = 0;
            Task* const t1 = task(1);
            Task* const t2 = task(2);
            Task* const t3 = task(3);
            Thread* const m1 = member(1, 2, t1);
            Thread* const m2 = member(2, 2, t2);
            Thread* const m3 = member(3, 2, t3);
            Thread* const a = seat_pool(4, 3);
            Thread* const b = seat_pool(5, 3);
            {
                IrqLock lock;
                park_mutex_waiter(m1, own_mutex(a, &wa));
                park_mutex_waiter(m2, own_mutex(b, &wb));
                park_death_waiter(a, t2);
                park_death_waiter(b, t3);
                ParkToken const ask = park_cancel_pending(c);
                (void)death_park(ask)(c, t1, lock);
                death_wait_lend(t1, lock);
            }
            EXPECT_EQ(a->prio, 9);
            EXPECT_EQ(m2->prio, 9) << "the first hop is within the bound";
            EXPECT_EQ(b->prio, 9);
            EXPECT_EQ(m3->prio, 2) << "the drain lent past its bound";
            EXPECT_TRUE(kernel().death_dirty.empty()) << "a waiter was left dirty";
            EXPECT_EQ(linked(kernel().death_waiters), 3) << "a waiter was lost or linked twice";
            {
                IrqLock lock;
                thread_abort_park(c, -KOS_ETIMEDOUT, lock);
            }
            EXPECT_TRUE(kernel().death_dirty.empty());
            EXPECT_EQ(linked(kernel().death_waiters), 2);
            EXPECT_EQ(m1->prio, 2);
        }
    }
}
