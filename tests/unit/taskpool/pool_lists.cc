// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The thread pool's claim order, its reclaimable list and each slot's children, and a task's
// holdings counted from its own members, each object once.

#include "../common/seam_pools.h"

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class PoolLists : public KSeam
            {
            };

            static_assert(KICKOS_SEAM_BUDGET_NOTIFICATIONS == 3,
                          "the holdings arms count notifications against the budget by hand");
            static_assert(KICKOS_SEAM_NOTIFY > KICKOS_SEAM_BUDGET_NOTIFICATIONS + 1,
                          "the holdings arms take one notification past the budget");

            constexpr uint8_t PRIO = 5;
            constexpr uint32_t CAP_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + KICKOS_SEAM_BUDGET_NOTIFICATIONS + 4;

            // A claimed slot, its occupant running, as a spawn's thread_create leaves it.
            int claim()
            {
                IrqLock lock;
                int const s = kernel().threads.alloc(nullptr);
                if (s >= 0)
                {
                    kernel().threads.slots()[s].state.to<ThreadState::READY>();
                }
                return s;
            }

            void retire(int s)
            {
                IrqLock lock;
                kernel().threads.retire(&kernel().threads.slots()[s]);
            }

            // Slot `child`'s occupant was spawned by slot `parent`'s, as thread_create records it.
            void spawned_by(int child, int parent)
            {
                ThreadPool& p = kernel().threads;
                uint16_t const tag = ThreadPool::kill_tag_for_index(parent);
                p.slots()[child].spawner_tag = tag;
                p.child_link(tag, child);
            }

            Thread* member_of(Task* group, int slot)
            {
                Thread* const t = seat_pool(slot, PRIO);
                join_task(t, group);
                attach_caps(t, CAP_WIDTH);
                return t;
            }

            // A live notification nobody holds yet, by handle.
            int notification()
            {
                Kernel& k = kernel();
                int const i = k.notifies.alloc(nullptr);
                EXPECT_GE(i, 0) << "fixture: the notification pool is exhausted";
                *k.notifies.at(i) = Notification{};
                k.notifies.refs()[i] = 0;
                return k.notifies.handle_for(i);
            }

            void hold(Thread* t, int obj)
            {
                Kernel& k = kernel();
                k.notifies.refs()[k.notifies.live_index(obj)]++;
                uint32_t cap = KCAP_INVALID;
                IrqLock lock;
                ASSERT_EQ(cap_install(t, obj, CapType::CAP_NOTIFY, 0, &cap), 0)
                    << "fixture: the capability was seated";
            }

            bool admits_a_notification(Task const* group)
            {
                IrqLock lock;
                return task_admit(group, BudgetKind::NOTIFICATIONS, 1);
            }
        }

        TEST_F(PoolLists, fresh_slots_are_claimed_in_order_until_the_pool_refuses)
        {
            ThreadPool& p = kernel().threads;
            int const first = p.next;
            for (int s = first; s < p.count(); s++)
            {
                EXPECT_EQ(claim(), s);
            }
            EXPECT_EQ(claim(), -1) << "a full pool refuses";
        }

        TEST_F(PoolLists, the_slot_that_exited_last_is_reclaimed_first)
        {
            ThreadPool& p = kernel().threads;
            ASSERT_EQ(claim(), ThreadPool::ROOT_INDEX);
            int const a = claim();
            int const b = claim();
            int const c = claim();
            ASSERT_GE(c, 0);
            retire(a);
            retire(c);
            retire(ThreadPool::ROOT_INDEX);
            uint16_t const gen_c = p.gen()[c];

            EXPECT_EQ(claim(), c) << "the last one retired";
            EXPECT_EQ(p.gen()[c], static_cast<uint16_t>(gen_c + 1)) << "a reclaim kills its handles";
            EXPECT_EQ(claim(), a);
            EXPECT_EQ(claim(), c + 1) << "then a fresh slot, never root's";
            (void)b;
        }

        TEST_F(PoolLists, a_full_pool_reclaims_its_one_exited_slot_and_refuses_after)
        {
            ThreadPool& p = kernel().threads;
            int top = -1;
            while (true)
            {
                int const s = claim();
                if (s < 0)
                {
                    break;
                }
                top = s;
            }
            ASSERT_GT(top, ThreadPool::ROOT_INDEX);
            retire(top);
            EXPECT_EQ(claim(), top);
            EXPECT_EQ(claim(), -1);
            EXPECT_EQ(p.next, p.count());
        }

        TEST_F(PoolLists, a_released_reclaim_keeps_its_generation_and_is_claimed_next)
        {
            ThreadPool& p = kernel().threads;
            ASSERT_EQ(claim(), ThreadPool::ROOT_INDEX);
            int const a = claim();
            int const b = claim();
            retire(a);
            retire(b);
            uint16_t const gen_b = p.gen()[b];
            int got = -1;
            {
                IrqLock lock;
                got = p.alloc(nullptr);
                ASSERT_EQ(got, b);
                p.release(got);
            }
            EXPECT_EQ(p.gen()[b], gen_b) << "the prior occupant's handle still resolves";
            EXPECT_EQ(claim(), b) << "and the slot is the next claim again";
            EXPECT_EQ(claim(), a);
        }

        TEST_F(PoolLists, a_reclaim_orphans_the_children_of_its_slot_and_no_other)
        {
            ThreadPool& p = kernel().threads;
            ASSERT_EQ(claim(), ThreadPool::ROOT_INDEX);
            int const parent = claim();
            int const other = claim();
            int const kept = claim();
            int const gone = claim();
            int const moved = claim();
            ASSERT_GE(moved, 0);
            spawned_by(kept, parent);
            spawned_by(gone, parent);
            spawned_by(moved, parent);

            // `moved` dies and its slot goes to a thread `other` spawned.
            retire(moved);
            ASSERT_EQ(claim(), moved);
            spawned_by(moved, other);

            retire(parent);
            ASSERT_EQ(claim(), parent);
            EXPECT_EQ(p.slots()[kept].spawner_tag, ThreadPool::KILL_TAG_NONE)
                << "a child of the reclaimed slot is orphaned";
            EXPECT_EQ(p.slots()[gone].spawner_tag, ThreadPool::KILL_TAG_NONE);
            EXPECT_EQ(p.slots()[moved].spawner_tag, ThreadPool::kill_tag_for_index(other))
                << "a slot that left the list keeps its new spawner";
            EXPECT_TRUE(p.slots()[parent].children.empty()) << "the reclaimed slot has no children";

            retire(other);
            ASSERT_EQ(claim(), other);
            EXPECT_EQ(p.slots()[moved].spawner_tag, ThreadPool::KILL_TAG_NONE)
                << "the other spawner's reclaim orphans its own child";
        }

        TEST_F(PoolLists, root_never_orphans_its_children)
        {
            ThreadPool& p = kernel().threads;
            ASSERT_EQ(claim(), ThreadPool::ROOT_INDEX);
            int const child = claim();
            spawned_by(child, ThreadPool::ROOT_INDEX);
            retire(ThreadPool::ROOT_INDEX);
            EXPECT_NE(claim(), ThreadPool::ROOT_INDEX);
            EXPECT_EQ(p.slots()[child].spawner_tag,
                      ThreadPool::kill_tag_for_index(ThreadPool::ROOT_INDEX));
        }

        TEST_F(PoolLists, an_object_held_twice_counts_once)
        {
            Task* const group = task(0);
            Thread* const a = member_of(group, 1);
            Thread* const b = member_of(group, 2);
            int const first = notification();
            int const second = notification();
            hold(a, first);
            hold(a, second);
            hold(a, first);
            hold(b, first);
            hold(b, second);
            EXPECT_TRUE(admits_a_notification(group))
                << "two distinct notifications under a budget of three, however many names";

            hold(b, notification());
            EXPECT_FALSE(admits_a_notification(group)) << "a third distinct one is the ceiling";
        }

        TEST_F(PoolLists, a_member_leaving_takes_what_only_it_held)
        {
            Task* const group = task(0);
            Thread* const a = member_of(group, 1);
            Thread* const b = member_of(group, 2);
            int const shared = notification();
            hold(a, shared);
            hold(a, notification());
            hold(b, shared);
            hold(b, notification());
            ASSERT_FALSE(admits_a_notification(group)) << "fixture: the task is at its ceiling";

            {
                IrqLock lock;
                task_member_leave(b, 2);
            }
            EXPECT_EQ(b->task, nullptr);
            EXPECT_TRUE(admits_a_notification(group))
                << "the object only the leaver held is no longer the task's";

            uint32_t members = 0;
            for (ListNode* m = task_members(group); m != nullptr; m = m->next)
            {
                members++;
                EXPECT_EQ(KICKOS_CONTAINER_OF(m, Thread, member), a) << "a is the only member left";
            }
            EXPECT_EQ(members, 1u);
        }

        TEST_F(PoolLists, a_bound_notification_and_a_capability_on_it_count_once)
        {
            Task* const group = task(0);
            Thread* const a = member_of(group, 1);
            int const bound = notification();
            hold(a, bound);
            a->notify_bound = notify_bound_store(bound);
            hold(a, notification());
            EXPECT_TRUE(admits_a_notification(group));
        }
    }
}
