// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A member sweeps its capabilities after it leaves its task, across lock gaps, so the task
// EMPTY and the task SWEPT are two moments apart. Two things wait for the second:
//   * the DEAD a watch on the task reports, which a restart follows, also where the creator's
//     death dropped the hold and the slot was freed under the sweep;
//   * the creator's hold a slay leaves, which goes with that DEAD;
//   * the slot itself, which a hold dropped by its creator's death frees at once, while a member
//     of the old instance still sweeps under its name and so still counts in the budget of
//     whatever task the slot holds next.
// The other member's whole exit runs inside the first one's sweep gap, as it does on a second
// core or when a member the sweep wakes outranks the sweeper.
//
// Then what dropping the hold does to a running task's members, and the end's group scan.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
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
            constexpr int SLOT_CREATOR = 4;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;
            constexpr uint8_t PRIO_HIGH = 7;

            // Several chunks, so the sweep opens gaps past its first one.
            constexpr uint32_t HEAVY_WIDTH = KICKOS_SEAM_ROOT_WIDTH;
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
                uint32_t pending;
                Task* restart;
                bool straggler;
                Thread* heavy;
                uint8_t heavy_prio;
                Thread* creator;
                int state;
            };
            MidSweep g_mid;

            void light_exits()
            {
                g_mid.sweep_open = cap_teardown_active();
                run_exit_in_gap(g_mid.light, 0);
                g_mid.members = task_member_count(g_mid.group);
                g_mid.sweeping = task_sweeping(g_mid.group);
                g_mid.waiter_parked = (g_mid.waiter->wait_kind == WAIT_NOTIFY);
                g_mid.pending = notify_pending_of(g_mid.waiter);
            }

            // As a creator's death drops the hold while a member still sweeps: the slot is freed
            // at once.
            void light_exits_and_the_hold_goes()
            {
                light_exits();
                IrqLock lock;
                task_drop_hold(g_mid.group, lock);
                g_mid.waiter_parked = (g_mid.waiter->wait_kind == WAIT_NOTIFY);
                g_mid.pending = notify_pending_of(g_mid.waiter);
            }

            void light_exits_and_the_hold_goes_under_a_lend()
            {
                light_exits_and_the_hold_goes();
                g_mid.heavy_prio = g_mid.heavy->prio;
            }

            // What a creator's death does at any time, or a slay at the death: the hold goes, and the
            // creator creates again.
            void light_exits_and_the_creator_restarts()
            {
                g_mid.sweep_open = cap_teardown_active();
                run_exit_in_gap(g_mid.light, 0);
                IrqLock lock;
                task_drop_hold(g_mid.group, lock);
                int err = 0;
                g_mid.restart = kickos::testfix::task_held(FIXTURE_TASK_TAG, &err, lock);
                g_mid.straggler = false;
                Kernel& k = kernel();
                for (int s = 0; g_mid.restart != nullptr and s < k.threads.next; s++)
                {
                    if (k.threads.slots()[s].task == g_mid.restart)
                    {
                        g_mid.straggler = true;
                    }
                }
            }

            int state_for_creator(Task* group)
            {
                Thread* const was = kernel().current(kickos_kernel_core());
                kernel().current(kickos_kernel_core()) = g_mid.creator;
                int32_t status = 0;
                int const state = task_state_call(task_handle(group), &status);
                kernel().current(kickos_kernel_core()) = was;
                return state;
            }

            void light_exits_and_the_creator_reads_the_state()
            {
                light_exits();
                g_mid.state = state_for_creator(g_mid.group);
            }

            // Two members of `group` slain together, the heavy one first out.
            Task* pair_in(Task* group, Thread** heavy, Thread** light)
            {
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

            Task* slain_pair(Thread** heavy, Thread** light)
            {
                return pair_in(task(0), heavy, light);
            }
        }

        // The death is reported when the last member's sweep is done, not when the last member
        // leaves: woken at the emptying, its waiter restarted into a ring block or a stack the
        // heavy member's sweep had not yet given back.
        TEST_F(SweepWait, a_death_s_watch_is_raised_once_every_member_has_swept)
        {
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            Task* const group = slain_pair(&heavy, &light);
            Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_LOW);
            park_watch_waiter(waiter, group);
            g_mid.waiter = waiter;
            run_in_chunk_gap(light_exits, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the light member left inside the heavy one's sweep";
            ASSERT_EQ(g_mid.members, 0u) << "and its exit emptied the group";
            ASSERT_EQ(g_mid.sweeping, 1u) << "while the heavy member still swept";
            EXPECT_TRUE(g_mid.waiter_parked) << "so the emptying woke the waiter";
            EXPECT_EQ(g_mid.pending, 0u) << "and raised nothing";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the last sweep did not wake it";
            EXPECT_EQ(notify_pending_of(waiter), 1u) << "nor raise the death";
            EXPECT_EQ(task_sweeping(group), 0u);
        }

        // A creator's death drops the hold and the slot is freed while a member still sweeps under
        // it: the watch outlives the slot until that sweep is done, reports the death then, and
        // goes with it. Cleared with the slot, the death of a task whose creator died reached
        // nobody.
        TEST_F(SweepWait, a_slot_freed_mid_sweep_keeps_its_death_s_watch_until_the_last_sweep)
        {
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            Task* const group = slain_pair(&heavy, &light);
            Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_LOW);
            park_watch_waiter(waiter, group);
            g_mid.waiter = waiter;
            uint16_t const gen = task_gen(group);
            run_in_chunk_gap(light_exits_and_the_hold_goes, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the hold went inside the heavy member's sweep";
            ASSERT_NE(task_gen(group), gen) << "fixture: the dropped hold did not free the slot";
            EXPECT_TRUE(g_mid.waiter_parked) << "the freed slot woke the waiter";
            EXPECT_EQ(g_mid.pending, 0u) << "the freed slot raised the death mid-sweep";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the last sweep did not wake the waiter";
            EXPECT_EQ(notify_pending_of(waiter), 1u) << "the death of the freed slot was lost";
            EXPECT_EQ(group->watch_notify, 0) << "the watch outlived the last sweep";
        }

        // The waiter on the freed slot's death keeps lending to the member still sweeping under
        // it, for as long as the kept watch stands.
        TEST_F(SweepWait, a_slot_freed_mid_sweep_keeps_the_lend_of_its_death_s_watch)
        {
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            Task* const group = slain_pair(&heavy, &light);
            Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_HIGH);
            park_watch_waiter(waiter, group);
            g_mid.waiter = waiter;
            g_mid.heavy = heavy;
            {
                IrqLock lock;
                watch_lend_task(group, lock);
            }
            ASSERT_EQ(heavy->prio, PRIO_HIGH) << "fixture: the waiter lends nothing";
            uint16_t const gen = task_gen(group);
            run_in_chunk_gap(light_exits_and_the_hold_goes_under_a_lend, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the hold went inside the heavy member's sweep";
            ASSERT_NE(task_gen(group), gen) << "fixture: the dropped hold did not free the slot";
            ASSERT_TRUE(g_mid.waiter_parked) << "fixture: the freed slot woke the waiter";
            EXPECT_EQ(g_mid.heavy_prio, PRIO_HIGH) << "the freed slot ended the lend while its member swept";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the last sweep did not wake the waiter";
        }

        // The slayer reads the task's state through its sweep: the hold goes with the death's
        // report, which is when the handle stops naming the task. Dropped at the slay, a waiter on
        // a bit several sources share could not tell which one the death's raise was.
        TEST_F(SweepWait, a_slain_task_reads_ended_until_its_death_is_reported_then_names_nothing)
        {
            Thread* const creator = seat_pool(SLOT_CREATOR, PRIO_LOW);
            Task* group = nullptr;
            {
                int err = 0;
                IrqLock lock;
                group = task_held(kernel().threads.kill_tag_of(creator), &err, lock);
            }
            ASSERT_NE(group, nullptr);
            Thread* heavy = nullptr;
            Thread* light = nullptr;
            pair_in(group, &heavy, &light);
            g_mid.creator = creator;
            Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_LOW);
            park_watch_waiter(waiter, group);
            g_mid.waiter = waiter;
            uint16_t const gen = task_gen(group);
            {
                IrqLock lock;
                task_stop_for_slay(group, lock);
            }
            EXPECT_EQ(state_for_creator(group), KOS_TASK_LIVE | KOS_TASK_ENDED)
                << "the slay let go of a task whose members still run";
            {
                IrqLock lock;
                task_stop_for_slay(group, lock);
            }
            EXPECT_EQ(state_for_creator(group), KOS_TASK_LIVE | KOS_TASK_ENDED)
                << "a second slay let go of it";
            run_in_chunk_gap(light_exits_and_the_creator_reads_the_state, GAP_MID_SWEEP);

            run_exit(0);

            ASSERT_TRUE(g_mid.sweep_open) << "the light member left inside the heavy one's sweep";
            ASSERT_EQ(g_mid.sweeping, 1u) << "fixture: the heavy member was not sweeping";
            EXPECT_EQ(g_mid.state, KOS_TASK_ENDED) << "the hold went before the last sweep";
            EXPECT_EQ(g_mid.pending, 0u) << "the death was raised mid-sweep";
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE) << "the last sweep did not wake the waiter";
            EXPECT_EQ(notify_pending_of(waiter), 1u) << "the death went unreported";
            EXPECT_EQ(state_for_creator(group), -KOS_EBADF) << "the hold outlived the death's report";
            EXPECT_NE(task_gen(group), gen) << "and the slot did not go with it";
            EXPECT_EQ(group->watch_notify, 0) << "nor the watch";
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
            Task const& slot = kernel().tasks()[0];
            EXPECT_EQ(slot.sweeping, 0u) << "and the slot is free once the sweep is done";
            EXPECT_EQ(slot.refcount, 0u);
        }

        // A task that has not ended keeps running when its creator lets go.
        TEST_F(SweepWait, dropping_the_hold_on_a_running_task_cancels_nobody)
        {
            Task* const group = task(0);
            Thread* const member = seat_pool(SLOT_LIGHT, PRIO_LOW);
            join_task(member, group);
            static uint32_t word;
            word = 0;
            park_word_waiter(member, plain_word(&word));

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
            entry->task_role = kickos::TASK_ROLE_ENTRY;
            stray->task = group;
            kernel().current(kickos_kernel_core()) = entry;

            run_exit(0);

            ASSERT_TRUE(task_ended(group)) << "the entry's exit ended the task";
            EXPECT_EQ(stray->cancel_kind, CANCEL_NONE);
        }
    }
}
