// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SAME death path as leave_space.cc, compiled at ONE kernel core: the arm of exit_current
// that reroots onto the boot root is guarded on KICKOS_KERNEL_CORES > 1, and this gate is what
// holds the guard on.
//
// The invariant it rests on: at one core the only walker of a space's tables is this core, and
// the release that frees them (kernel/mem/aspace.cc, aspace_release) activates the boot root on
// the releasing core BEFORE arch_aspace_destroy. So the reroot here would buy nothing and spend
// a full non-tagged flush, plus a second one on the next sibling's switch-in, per thread death.
//
// WHAT THIS GATE CANNOT WITNESS, stated as a limit: aspace_release itself, which is a
// kernel/mem translation unit no host board compiles (aspace_seam.cc stands in for that whole
// layer and defines no release). The order inside it is read from the source and witnessed on
// the translating targets.

#include <string.h>

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LeaveSpaceOneCore : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    aspace_seam_reset();
                }
            };

            static_assert(KICKOS_KERNEL_CORES == 1,
                          "this gate reads the arm compiled OUT at more than one core");
            static_assert(KICKOS_HAVE_ASPACE,
                          "at KICKOS_HAVE_ASPACE 0 exit_current has no aspace arm at all");

            // The dying and surviving threads take non-root slots; slot 0 is root's and stands
            // in here as the bystander that never runs.
            constexpr int SLOT_BYSTANDER = 0;
            constexpr int SLOT_DYING = 1;
            constexpr int SLOT_SURVIVOR = 2;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            constexpr uintptr_t STACK_BASE = 0x40000000u;
            constexpr size_t STACK_BYTES = 0x4000u;

            bool traced(char const* token)
            {
                return strstr(trace(), token) != nullptr;
            }

            Thread* member_of(int slot, uint8_t prio, Task* group)
            {
                Thread* const t = seat_pool(slot, prio);
                join_task(t, group);
                return t;
            }

            void own_stack(Thread* t)
            {
                t->kstack_owned = true;
                t->stack_base = reinterpret_cast<void*>(STACK_BASE);
                t->stack_size = STACK_BYTES;
            }

            // One core, so arch_cpu_id() is the literal 0 and kfixture's g_core does not
            // exist at all (kfixture.h compiles it out below one core).
            void run_as(Thread* t)
            {
                kernel().current[0] = t;
                t->state = ThreadState::RUNNING;
                install_here(space_of(task_domain(t->task)));
            }
        }

        // A NON-LAST member's death: the group keeps a member, so no release can reach these
        // tables and nothing has to leave them.
        TEST_F(LeaveSpaceOneCore, a_non_last_member_death_does_not_reroot_this_core)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            ASSERT_NE(group, nullptr);
            Thread* const dying = member_of(SLOT_DYING, PRIO_MID, group);
            Thread* const survivor = member_of(SLOT_SURVIVOR, PRIO_MID, group);
            own_stack(dying);
            ASSERT_EQ(task_member_count(group), 2);

            struct arch_aspace* const space = space_of(task_domain(group));
            run_as(dying);
            ASSERT_EQ(installed_on(0), space);
            trace_reset();
            run_exit(0);

            ASSERT_TRUE(traced("ustack_free"))
                << "the arm did not reach ustack_free: trace " << trace();
            // No `task_release` token here: the seam traces the DOMAIN release, which a
            // non-last member's task_release does not reach.
            EXPECT_TRUE(traced("ustack_free(seated)"))
                << "ustack_free must still run against the installed space: trace " << trace();
            EXPECT_FALSE(traced("install_boot"))
                << "at one core the reroot buys nothing and costs a full non-tagged flush "
                   "here and a second on the next sibling's switch-in: trace "
                << trace();
            EXPECT_EQ(task_member_count(group), 1)
                << "the group must still hold the survivor, or this is a LAST-member death "
                   "and the release path is the one that reroots";
            EXPECT_EQ(installed_on(0), space)
                << "the core stays on the space the surviving member still holds";
            (void)survivor;
        }

        // The LAST member's death, for the discriminator the arm above needs: the trace is the
        // same, the reroot being the release path's and not exit_current's on either count.
        TEST_F(LeaveSpaceOneCore, a_last_member_death_does_not_reroot_it_either)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            ASSERT_NE(group, nullptr);
            Thread* const dying = member_of(SLOT_DYING, PRIO_MID, group);
            own_stack(dying);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);
            ASSERT_EQ(task_member_count(group), 1);

            run_as(dying);
            trace_reset();
            run_exit(0);

            ASSERT_TRUE(traced("task_release"))
                << "the arm did not reach task_release: trace " << trace();
            EXPECT_EQ(task_member_count(group), 0)
                << "the group is empty, so the destroy precondition holds";
            EXPECT_FALSE(traced("install_boot")) << "trace " << trace();
        }

        // A thread whose task holds no space: the switch-in seam is where the boot root goes in
        // for one of these, not the exit path.
        TEST_F(LeaveSpaceOneCore, a_thread_holding_no_space_reroots_nothing_on_its_way_out)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Thread* const dying = seat_pool(SLOT_DYING, PRIO_MID);
            ASSERT_EQ(dying->task, nullptr);

            kernel().current[0] = dying;
            install_here(boot_space());
            trace_reset();
            run_exit(0);

            EXPECT_EQ(g_ustack_frees, 0u) << "no run of its own, so nothing to give back";
            EXPECT_EQ(installed_on(0), boot_space());
        }
    }
}
