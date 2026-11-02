// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// task_admit over the usage each kind derives from what the task holds: THREADS from its members
// and the members still sweeping, RAM from every record its declaration's record pays for, dead
// incarnations included.

#include "../common/seam_pools.h"

#include <kickos/budget.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/pool_storage.h>
#include <kickos/ramown.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class BudgetAdmit : public KSeam
            {
            };

            constexpr uint32_t THREADS = KICKOS_SEAM_BUDGET_THREADS;
            constexpr uint32_t BLOCKS = KICKOS_SEAM_BUDGET_RAM_BLOCKS;
            constexpr uint32_t BYTES = KICKOS_SEAM_BUDGET_RAM_BYTES;
            static_assert(THREADS == 4 and BLOCKS == 3 and BYTES == 4096,
                          "the arms count against this posture's budgets by hand");
            static_assert(KICKOS_SEAM_THREAD_SLOTS > THREADS + 2,
                          "the pool has slots past the budget, so a refusal is the budget's");

            bool admits(Task const* t, BudgetKind kind, uint32_t n)
            {
                IrqLock lock;
                return task_admit(t, kind, n);
            }

            uint32_t usage(Task const* t, BudgetKind kind)
            {
                IrqLock lock;
                return task_usage(t, kind);
            }

            Thread* member_of(Task* group, int slot)
            {
                Thread* const t = seat_pool(slot, 5);
                join_task(t, group);
                return t;
            }

#if not KICKOS_HAVE_ASPACE
            // A block of `size` bytes recorded as `owner`'s, as ram_owner_alloc records it.
            void record_block(Task const* owner, uint32_t size)
            {
                IrqLock lock;
                RamBlock* const blocks = pool_local(kickos_kernel_pools.ram_owners);
                int at = 0;
                while (blocks[at].size != 0)
                {
                    at++;
                }
                uint16_t* const head = task_ram_head(owner);
                blocks[at] = RamBlock{0x1000u * static_cast<uintptr_t>(at + 1), size,
                                      task_handle(owner)};
                blocks[at].next = *head;
                *head = static_cast<uint16_t>(at + 1);
            }
#endif
        }

        TEST_F(BudgetAdmit, the_kernel_s_own_claim_is_admitted)
        {
            EXPECT_TRUE(admits(nullptr, BudgetKind::THREADS, UINT32_MAX));
        }

        TEST_F(BudgetAdmit, a_claim_is_admitted_up_to_the_budget_and_refused_past_it)
        {
            Task* const tk = task(0);
            member_of(tk, 1);
            member_of(tk, 2);
            ASSERT_EQ(usage(tk, BudgetKind::THREADS), 2u);
            EXPECT_TRUE(admits(tk, BudgetKind::THREADS, THREADS - 2u));
            EXPECT_FALSE(admits(tk, BudgetKind::THREADS, THREADS - 1u))
                << "one past the budget, with the pool's slots to spare";
            EXPECT_FALSE(admits(tk, BudgetKind::THREADS, UINT32_MAX)) << "and no wrap";
        }

        TEST_F(BudgetAdmit, a_member_released_still_counts_until_its_sweep_is_done)
        {
            Task* const tk = task(0);
            member_of(tk, 1);
            Thread* const leaving = member_of(tk, 2);
            uint16_t const gen = task_gen(tk);
            {
                IrqLock lock;
                (void)task_release(tk, lock);
            }
            EXPECT_EQ(usage(tk, BudgetKind::THREADS), 2u) << "a member mid-sweep holds its slot";
            EXPECT_FALSE(admits(tk, BudgetKind::THREADS, THREADS - 1u));
            {
                IrqLock lock;
                (void)task_sweep_done(tk, gen);
            }
            EXPECT_EQ(usage(tk, BudgetKind::THREADS), 1u);
            EXPECT_TRUE(admits(tk, BudgetKind::THREADS, THREADS - 1u));
            (void)leaving;
        }

#if not KICKOS_HAVE_ASPACE
        TEST_F(BudgetAdmit, ram_is_the_sum_of_every_incarnation_of_the_record)
        {
            Task* const first = task(0);
            kos_task_t const was = task_handle(first);
            record_block(first, 1024u);
            record_block(first, 1024u);
            int err = 0;
            Task* again = nullptr;
            {
                IrqLock lock;
                again = task_create_declared(static_cast<size_t>(KICKOS_SEAM_TASKS - 1), 0,
                                             FIXTURE_TASK_TAG, nullptr, 0, 0u, nullptr, &err,
                                             lock);
            }
            ASSERT_EQ(again, first) << "the record a restart seats again";
            ASSERT_NE(task_handle(again), was) << "a new incarnation, under a new handle";
            EXPECT_EQ(usage(again, BudgetKind::RAM_BLOCKS), 2u);
            EXPECT_EQ(usage(again, BudgetKind::RAM_BYTES), 2048u);
            EXPECT_TRUE(admits(again, BudgetKind::RAM_BLOCKS, 1u));
            EXPECT_FALSE(admits(again, BudgetKind::RAM_BYTES, BYTES - 2047u));
            record_block(again, 1024u);
            EXPECT_FALSE(admits(again, BudgetKind::RAM_BLOCKS, 1u))
                << "the dead incarnation's blocks still count against the lifetime budget";
            EXPECT_TRUE(admits(again, BudgetKind::RAM_BYTES, BYTES - 3072u));
        }

        TEST_F(BudgetAdmit, another_record_s_blocks_are_not_its_own)
        {
            Task* const a = task(0);
            Task* const b = task(1);
            record_block(a, 1024u);
            record_block(a, 1024u);
            record_block(a, 1024u);
            EXPECT_FALSE(admits(a, BudgetKind::RAM_BLOCKS, 1u));
            EXPECT_EQ(usage(b, BudgetKind::RAM_BLOCKS), 0u);
            EXPECT_TRUE(admits(b, BudgetKind::RAM_BLOCKS, BLOCKS));
        }
#endif
    }
}
