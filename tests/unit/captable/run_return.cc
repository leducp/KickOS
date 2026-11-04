// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A task's capability runs over a slab that is exactly the sum of what the budgets back: the init's
// width and the task's CAP_SLOTS. A run is charged from its attach to the end of its thread's
// sweep, and goes back to the slab there, not when the slot is next claimed, so a task within its
// budget always finds every chunk it may take, whatever widths its threads asked.

#include "../common/seam_pools.h"

#include <kickos/budget.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/pools.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class RunReturn : public KSeam
            {
            };

            constexpr uint32_t SLOTS = KICKOS_SEAM_BUDGET_CAP_SLOTS;
            static_assert(SLOTS == 32 and KICKOS_SEAM_ROOT_WIDTH == 8,
                          "the arms count chunks against this posture's budget by hand");
            static_assert(KICKOS_SEAM_CAP_SLOTS == KICKOS_SEAM_ROOT_WIDTH + SLOTS,
                          "the slab is the init's width and the task's budget, nothing more");

            constexpr int SLOT_BYSTANDER = 0;
            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            uint32_t usage(Task const* t)
            {
                IrqLock lock;
                return task_usage(t, BudgetKind::CAP_SLOTS);
            }

            bool admits(Task const* t, uint32_t width)
            {
                IrqLock lock;
                return task_admit(t, BudgetKind::CAP_SLOTS, kcap_width_for(width));
            }

            // A member of `group` in pool slot `slot` holding a run of `width`, admitted as a
            // spawn admits it. The fixture fails the arm where the slab cannot seat it.
            Thread* member(Task* group, int slot, uint32_t width)
            {
                EXPECT_TRUE(admits(group, width)) << "slot " << slot << " width " << width;
                Thread* const t = seat_pool(slot, PRIO_MID);
                join_task(t, group);
                attach_caps(t, width);
                return t;
            }

            void exits(Thread* t)
            {
                kernel().current(kickos_kernel_core()) = t;
                run_exit(0);
            }
        }

        TEST_F(RunReturn, the_slab_is_the_sum_of_the_budgets_it_backs)
        {
            EXPECT_EQ(kickos_kernel_pools.cap_chunk_count * KCAP_CHUNK_SLOTS,
                      uint32_t{KICKOS_SEAM_CAP_SLOTS});
            EXPECT_EQ(kickos_kernel_pools.cap_root_width, uint32_t{KICKOS_SEAM_ROOT_WIDTH});
        }

        TEST_F(RunReturn, a_run_is_charged_in_whole_chunks_from_its_attach)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            (void)member(group, 1, 3);
            EXPECT_EQ(usage(group), 8u) << "a 3-slot table takes a whole chunk";
            (void)member(group, 2, 9);
            EXPECT_EQ(usage(group), 24u);
            EXPECT_TRUE(admits(group, 8));
            EXPECT_FALSE(admits(group, 9)) << "one past what remains of the budget";
        }

        // Widths that shrink, grow and come back: each exit gives its chunks back before the
        // next spawn, so the replacement may be wider than any one worker that left. The keeper
        // stays, as a task's entry does while its workers come and go.
        TEST_F(RunReturn, a_run_returns_at_the_end_of_its_sweep_not_at_reclaim)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            (void)member(group, 1, 8);
            Thread* const a = member(group, 2, 16);
            Thread* const b = member(group, 3, 8);
            ASSERT_EQ(usage(group), SLOTS);
            EXPECT_FALSE(admits(group, 1)) << "the budget is spent";

            exits(a);
            EXPECT_EQ(usage(group), 16u) << "the exited worker's run left the charge with its sweep";
            Thread* const c = member(group, 4, 3);
            Thread* const d = member(group, 5, 8);
            ASSERT_EQ(usage(group), SLOTS);

            exits(b);
            exits(c);
            exits(d);
            EXPECT_EQ(usage(group), 8u);
            // Wider than any worker that left, with none of their slots claimed again: every
            // chunk is back on the slab's list.
            (void)member(group, 6, SLOTS - 8u);
            EXPECT_EQ(usage(group), SLOTS);
        }
    }
}
