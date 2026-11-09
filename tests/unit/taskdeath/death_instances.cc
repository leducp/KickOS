// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The death-wait leave batch is instance-scoped: under the multi-instance sim, a batch one
// instance's timer interrupt holds open defers no leave of another instance's.

#include <kickos/instance.h>
#include <kickos/instance_local.h>
#include <kickos/irqlock.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class DeathInstances : public KSeam
            {
            };

            constexpr uint8_t MEMBER_PRIO = 2;
            constexpr uint8_t WAITER_PRIO = 9;
        }

        TEST_F(DeathInstances, a_batch_open_in_one_instance_defers_no_leave_of_another)
        {
            {
                IrqLock lock;
                death_leave_batch_begin();
            }
            unsigned const home = instance_select(1u);
            reset();
            Task* const t = task(1);
            Thread* const m = seat_pool(1, MEMBER_PRIO);
            attach_caps(m, KICKOS_SEAM_CAP_WIDTH);
            join_task(m, t);
            Thread* const w = seat_pool(2, WAITER_PRIO);
            {
                IrqLock lock;
                park_death_waiter(w, t);
                death_wait_lend(t, lock);
            }
            uint8_t const lent = m->prio;
            {
                IrqLock lock;
                death_wait_leave(w, lock);
            }
            uint8_t const left = m->prio;
            (void)instance_select(home);
            {
                IrqLock lock;
                death_leave_batch_end(lock);
            }
            ASSERT_EQ(lent, WAITER_PRIO) << "fixture: the waiter lends the member nothing";
            EXPECT_EQ(left, MEMBER_PRIO)
                << "the member kept the lend of a waiter gone: the leave waited on a batch "
                   "another instance opened";
        }
    }
}
