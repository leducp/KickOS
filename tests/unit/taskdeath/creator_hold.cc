// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The creator's hold on an EXPLICIT task, and what a creator's death does to it
// (docs/design-task-layer.md section 9.4).
//
// WHY THIS IS AN AUTHORITY GATE AND NOT A SLOT-LEAK ONE. A kill tag is DERIVED from the pool
// slot (kill_tag_for_index, thread.h), so the thread that next occupies a dead creator's slot
// answers kill_tag_of with the dead creator's tag. Every gate over an explicit task reads
// task_created_by against that tag: killing it, slaying it, and seating a member, which
// hands that member the group's domain regions. A hold left behind is therefore authority the
// successor never earned, and the generation bump that kills a stale handle does not reach it:
// a slot with a live member is not freed.
//
// WHAT THIS GATE CANNOT WITNESS: the syscall refusals themselves. syscall_thread.cc is not in
// the K-seam source set, so the -KOS_EPERM a stranger gets and the -KOS_EBADF a dead group's
// handle gets are the selftest's to show. The subject here is the predicate all three of those
// refusals read, and the reference and slot bookkeeping under it.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class CreatorHold : public KSeam
            {
            };

            // Pool slots. The creator is NOT slot 0: root is, and an arm that reuses root's
            // slot would be about ROOT_INDEX rather than about the tag scheme.
            constexpr int SLOT_BYSTANDER = 0;
            constexpr int SLOT_CREATOR = 1;
            constexpr int SLOT_MEMBER = 2;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            // A live thread the exit path can leave behind: exit_current calls
            // kickos_terminate when the dying thread was the last one out, and the seam
            // stub ends the arm there.
            Thread* bystander()
            {
                return seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            }

            Task* create_for(Thread* creator)
            {
                int err = 0;
                return task_create(kernel().threads.kill_tag_of(creator), nullptr, 0, 0u,
                                   /*caller_authorized=*/false, &err);
            }
        }

        // THE ESCAPE, stated as the predicate. Without the sweep in exit_current the group
        // keeps its dead creator's tag, and every later thread in that pool slot passes the
        // creator gate for it.
        TEST_F(CreatorHold, a_dead_creator_stops_naming_its_group)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            uint16_t const tag = kernel().threads.kill_tag_of(creator);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            Thread* const member = seat_pool(SLOT_MEMBER, PRIO_LOW);
            join_task(member, group);
            kos_task_t const handle = task_handle(group);
            ASSERT_TRUE(task_created_by(group, tag));
            ASSERT_EQ(task_resolve(handle), group);

            kernel().current = creator;
            run_exit(0);

            EXPECT_FALSE(task_created_by(group, tag))
                << "the hold ends with the creator: the tag it was keyed on is a SLOT's";
            EXPECT_EQ(task_resolve(handle), nullptr)
                << "and the group is unnameable, so no handle reaches it either";
            EXPECT_EQ(task_member_count(group), 1)
                << "its live member is untouched: a spawner's death is not a group kill";
        }

        // The successor half, and it does not depend on which slot a reclaim happens to
        // choose: seating the SAME slot is what makes the two tags equal by construction.
        TEST_F(CreatorHold, the_successor_of_a_dead_creators_slot_inherits_no_authority)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            uint16_t const tag = kernel().threads.kill_tag_of(creator);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            Thread* const member = seat_pool(SLOT_MEMBER, PRIO_LOW);
            join_task(member, group);

            kernel().current = creator;
            run_exit(0);

            Thread* const successor = seat_pool(SLOT_CREATOR, PRIO_MID);
            ASSERT_EQ(kernel().threads.kill_tag_of(successor), tag)
                << "the tag IS the slot, so the successor answers with its predecessor's";
            EXPECT_FALSE(task_created_by(group, kernel().threads.kill_tag_of(successor)))
                << "and that is exactly the authority it must not inherit";
        }

        // No member to hold it, so the drop frees the slot outright. The generation bump is
        // what stops the handle resolving onto the next task to land here.
        TEST_F(CreatorHold, an_empty_group_dies_with_its_creator)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            Domain* const dom = task_domain(group);
            kos_task_t const handle = task_handle(group);
            ASSERT_EQ(domain_refs(dom), 1u);

            kernel().current = creator;
            run_exit(0);

            EXPECT_EQ(task_resolve(handle), nullptr) << "the slot went back";
            EXPECT_EQ(domain_refs(dom), 0u) << "and so did its domain reference";
            Thread* const successor = seat_pool(SLOT_CREATOR, PRIO_MID);
            Task* const reused = create_for(successor);
            ASSERT_EQ(reused, group) << "the freed slot is the first one free_slot finds";
            EXPECT_EQ(task_resolve(handle), nullptr)
                << "and the stale handle still misses it: the generation moved";
        }

        // The other direction, and the mutant it kills is a sweep keyed on nothing: a death
        // that is not the creator's must leave the hold exactly where it was.
        TEST_F(CreatorHold, a_strangers_death_leaves_the_hold_alone)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            uint16_t const tag = kernel().threads.kill_tag_of(creator);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            kos_task_t const handle = task_handle(group);
            Thread* const stranger = seat_pool(SLOT_MEMBER, PRIO_LOW);

            kernel().current = stranger;
            run_exit(0);

            EXPECT_TRUE(task_created_by(group, tag)) << "a stranger's exit is not the creator's";
            EXPECT_EQ(task_resolve(handle), group) << "and the group is still nameable";
        }

        // EMPTIED IS NOT DEAD. The reservation is what lets an explicit task exist with no
        // member at all, which is the state kos_task_create leaves it in and the state a
        // group returns to between its last member and its creator's death.
        TEST_F(CreatorHold, an_emptied_group_outlives_its_members)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            uint16_t const tag = kernel().threads.kill_tag_of(creator);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            kos_task_t const handle = task_handle(group);
            Thread* const member = seat_pool(SLOT_MEMBER, PRIO_LOW);
            join_task(member, group);

            kernel().current = member;
            run_exit(0);

            ASSERT_EQ(task_member_count(group), 0) << "the group is empty";
            EXPECT_TRUE(task_created_by(group, tag)) << "and still its creator's to spawn into";
            EXPECT_EQ(task_resolve(handle), group) << "so the handle still resolves";

            kernel().current = creator;
            run_exit(0);

            EXPECT_EQ(task_resolve(handle), nullptr) << "the creator's death is what ends it";
        }

        // THE PRECONDITION, from the side that can only fail. task_orphan_created_by is
        // skipped outright while no hold is live (Kernel::task_holds), so what needs a gate is
        // that a sweep which is owed still runs, and for the second creator as much as the
        // first. A boolean, or a count the sweep clears rather than task_drop_hold, orphans
        // the first group and strands the second with its dead creator's tag.
        TEST_F(CreatorHold, a_second_creators_hold_outlives_the_first_creators_sweep)
        {
            bystander();
            Thread* const first = seat_pool(SLOT_CREATOR, PRIO_MID);
            uint16_t const first_tag = kernel().threads.kill_tag_of(first);
            Task* const first_group = create_for(first);
            ASSERT_NE(first_group, nullptr);
            Thread* const second = seat_pool(SLOT_MEMBER, PRIO_MID);
            uint16_t const second_tag = kernel().threads.kill_tag_of(second);
            Task* const second_group = create_for(second);
            ASSERT_NE(second_group, nullptr);
            ASSERT_NE(first_group, second_group);
            ASSERT_NE(first_tag, second_tag);

            kernel().current = first;
            run_exit(0);

            EXPECT_FALSE(task_created_by(first_group, first_tag))
                << "the sweep was owed and ran: the precondition does not skip a live hold";
            EXPECT_TRUE(task_created_by(second_group, second_tag))
                << "and it took only its own creator's, exactly as a stranger's death would";

            kernel().current = second;
            run_exit(0);

            EXPECT_FALSE(task_created_by(second_group, second_tag))
                << "the precondition still owes this one: the count is not one-shot";
        }

        // The creator hold and the members' hold are TWO references on ONE domain, and only
        // separating them keeps a domain alive across the order the two deaths arrive in.
        TEST_F(CreatorHold, the_creator_hold_is_a_domain_reference_of_its_own)
        {
            bystander();
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_MID);
            Task* const group = create_for(creator);
            ASSERT_NE(group, nullptr);
            Domain* const dom = task_domain(group);
            EXPECT_EQ(domain_refs(dom), 1u) << "the creator's, taken at create";
            Thread* const member = seat_pool(SLOT_MEMBER, PRIO_LOW);
            join_task(member, group);
            EXPECT_EQ(domain_refs(dom), 2u) << "the group's, taken when it became non-empty";

            kernel().current = creator;
            run_exit(0);

            EXPECT_EQ(domain_refs(dom), 1u)
                << "the creator's alone is dropped: the member is still running off these "
                   "regions";
            EXPECT_EQ(task_domain(group), dom) << "and the task still names the domain";

            kernel().current = member;
            run_exit(0);

            EXPECT_EQ(domain_refs(dom), 0u) << "the last member out returns it";
            EXPECT_EQ(task_domain(group), nullptr) << "and the freed slot names nothing";
        }
    }
}
