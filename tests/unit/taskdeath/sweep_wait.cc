// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A member sweeps its capabilities after it leaves its task, across lock gaps, so the task
// EMPTY and the task SWEPT are two moments apart. Two things wait for the second:
//   * kos_task_slay's park (WAIT_TASK_EMPTY), whose return drops the creator's hold and is
//     what a restart follows;
//   * the slot itself, which a hold dropped by a kill or by its creator's death frees at once,
//     while a member of the old instance still sweeps under its name and so still counts in
//     the budget of whatever task the slot holds next.
// The other member's whole exit runs inside the first one's sweep gap, as it does on a second
// core or when a member the sweep wakes outranks the sweeper.
//
// Then what dropping the hold does to a running task's members, and the end's group scan.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class SweepWait : public KSeam
            {
            };

            constexpr int SLOT_HEAVY = 0;
            constexpr int SLOT_LIGHT = 1;
            constexpr int SLOT_WAITER = 2;
            constexpr int SLOT_BYSTANDER = 3;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            // Several chunks, so the sweep opens gaps past its first one.
            constexpr uint32_t HEAVY_WIDTH = KICKOS_MAX_HANDLES;
            static_assert(HEAVY_WIDTH > 2 * KCAP_TEARDOWN_CHUNK,
                          "the heavy member's sweep must span three chunks");
            // The first gap closes the release bracket, before the sweep starts.
            constexpr uint32_t GAP_MID_SWEEP = 3;

            struct MidSweep
            {
                Thread* light;
                Thread* waiter;
                Task* group;
                bool sweep_open;
                uint8_t members;
                uint8_t sweeping;
                bool waiter_parked;
                Task* restart;
                bool straggler;
            };
            MidSweep g_mid;

            void light_exits()
            {
                g_mid.sweep_open = cap_teardown_active();
                run_exit_in_gap(g_mid.light, 0);
                g_mid.members = task_member_count(g_mid.group);
                g_mid.sweeping = task_sweeping(g_mid.group);
                g_mid.waiter_parked = (g_mid.waiter->wait_kind == WAIT_TASK_EMPTY);
            }

            // What the slay's return did before it waited for every sweep, and what a kill or a
            // creator's death does at any time: the hold goes, and the creator creates again.
            void light_exits_and_the_creator_restarts()
            {
                g_mid.sweep_open = cap_teardown_active();
                run_exit_in_gap(g_mid.light, 0);
                IrqLock lock;
                task_drop_hold(g_mid.group, lock);
                int err = 0;
                g_mid.restart = task_create(FIXTURE_TASK_TAG, nullptr, 0,
                                            /*mem_attr=*/0u, /*donor=*/nullptr, &err);
                g_mid.straggler = false;
                Kernel& k = kernel();
                for (int s = 0; g_mid.restart != nullptr and s < k.threads.next; s++)
                {
                    if (k.threads.slots[s].task == g_mid.restart)
                    {
                        g_mid.straggler = true;
                    }
                }
            }

            // Two members slain together, the heavy one first out.
            Task* slain_pair(Thread** heavy, Thread** light)
            {
                Task* const group = task(0);
                *heavy = seat_pool(SLOT_HEAVY, PRIO_MID);
                *light = seat_pool(SLOT_LIGHT, PRIO_MID);
                attach_caps(*heavy, HEAVY_WIDTH);
                join_task(*heavy, group);
                join_task(*light, group);
                (*heavy)->cancel_kind = CANCEL_SLAY;
                (*light)->cancel_kind = CANCEL_SLAY;
                g_mid = MidSweep{};
                g_mid.light = *light;
                g_mid.group = group;
                kernel().current(kickos_kernel_core()) = *heavy;
                return group;
            }
        }

        // The slay's park ends when the last member's sweep does, not when the last member
        // leaves: woken at the emptying, the slay returned and its caller restarted into a ring
        // block or a stack the heavy member's sweep had not yet given back.
        TEST_F(SweepWait, a_slay_is_woken_once_every_member_has_swept)
        {
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            Task* const group = slain_pair(&heavy, &light);
            Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_LOW);
            policy_on_remove(waiter);
            testfix::seat_blocked(waiter);
            waiter->wait_kind = WAIT_TASK_EMPTY;
            waiter->wait_obj = group;
            waiter->wait_result = WAIT_RESULT_POISON;
            g_mid.waiter = waiter;
            run_in_chunk_gap(light_exits, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the light member left inside the heavy one's sweep";
            ASSERT_EQ(g_mid.members, 0u) << "and its exit emptied the group";
            ASSERT_EQ(g_mid.sweeping, 1u) << "while the heavy member still swept";
            EXPECT_TRUE(g_mid.waiter_parked) << "so the emptying did not end the slay's park";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the last sweep ended it";
            EXPECT_EQ(waiter->wait_result, 0) << "with 0, the group empty and swept";
            EXPECT_EQ(task_sweeping(group), 0u);
        }

        // A slot is seated again only once no member sweeps under it. Seated earlier, the
        // straggler's capabilities count in the new task's budget for the rest of its sweep.
        TEST_F(SweepWait, a_restart_lands_on_no_slot_a_member_still_sweeps_under)
        {
            seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            Task* const group = slain_pair(&heavy, &light);
            run_in_chunk_gap(light_exits_and_the_creator_restarts, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the restart happened inside the heavy member's sweep";
            ASSERT_NE(g_mid.restart, nullptr) << "a spare slot took the restart";
            EXPECT_NE(g_mid.restart, group) << "not the slot the heavy member swept under";
            EXPECT_FALSE(g_mid.straggler) << "so no thread of the old instance counts in it";
            Task const& slot = kernel().tasks[0];
            EXPECT_EQ(slot.sweeping, 0u) << "and the slot is free once the sweep is done";
            EXPECT_EQ(slot.refcount, 0u);
        }

        // A task that has not ended keeps running when its creator lets go.
        TEST_F(SweepWait, dropping_the_hold_on_a_running_task_cancels_nobody)
        {
            Task* const group = task(0);
            Thread* const member = seat_pool(SLOT_LIGHT, PRIO_LOW);
            join_task(member, group);
            Semaphore* const s = semaphore(nullptr);
            park_sem_waiter(member, s);

            {
                IrqLock lock;
                task_orphan_created_by(FIXTURE_TASK_TAG, lock);
            }

            EXPECT_EQ(member->cancel_kind, CANCEL_NONE);
            EXPECT_EQ(member->state, ThreadState::BLOCKED) << "and left parked where it was";
        }

        // An entry that ends its task alone has no member to cancel, so the end scans no
        // thread. The stray names the group without counting in it, which no kernel path
        // leaves behind: only the scan reaches it, so it tells a skipped scan from one that
        // found nobody.
        TEST_F(SweepWait, an_entry_ending_alone_scans_no_thread)
        {
            Task* const group = task(0);
            Thread* const entry = seat_pool(SLOT_HEAVY, PRIO_MID);
            Thread* const stray = seat_pool(SLOT_LIGHT, PRIO_LOW);
            join_task(entry, group);
            entry->task_entry = true;
            stray->task = group;
            kernel().current(kickos_kernel_core()) = entry;

            run_exit(0);

            ASSERT_TRUE(task_ended(group)) << "the entry's exit ended the task";
            EXPECT_EQ(stray->cancel_kind, CANCEL_NONE);
        }
    }
}
