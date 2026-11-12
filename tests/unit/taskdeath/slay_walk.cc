// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The group walk against the first switch it can take. A member switched in before it carries
// the claim runs its own code after the slay, and a walker switched out before it has ended
// every park leaves members parked for good. So each arm reads the group at the first switch out
// of the walker: every live member marked, none still parked, and an incoming member claimed.
// The fixture books a switch and returns, as a deferred backend does, and runs the park waker the
// moment a parked thread is switched out, as an inline backend does.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
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
            class SlayWalk : public KSeam
            {
            };

            constexpr uint8_t PRIO_CREATOR = 8;
            constexpr uint8_t PRIO_WAKE = 6;
            constexpr uint8_t PRIO_WALKER = 4;
            constexpr uint8_t PRIO_MEMBER = 4;
            constexpr uint8_t PRIO_BELOW = 2;

            constexpr int MEMBERS_MAX = 4;

            struct Snap
            {
                bool taken;
                Thread* incoming;
                bool incoming_claimed;
                int members;
                Thread* member[MEMBERS_MAX];
                uint8_t kind[MEMBERS_MAX];
                ThreadState state[MEMBERS_MAX];
                // Set by an arm's action that ends the walk's work for it.
                ThreadState left_state;
            };
            Snap g_snap;
            Task* g_group;
            Thread* g_left;

            void watch(Task* group, Thread* const* members, int n)
            {
                g_snap = Snap{};
                g_group = group;
                g_left = nullptr;
                g_snap.members = n;
                for (int i = 0; i < n; i++)
                {
                    g_snap.member[i] = members[i];
                }
            }

            void take_snapshot()
            {
                if (g_snap.taken)
                {
                    return;
                }
                g_snap.taken = true;
                Thread* const in = sched::current();
                g_snap.incoming = in;
                g_snap.incoming_claimed = in->task != g_group or in->dying;
                for (int i = 0; i < g_snap.members; i++)
                {
                    g_snap.kind[i] = g_snap.member[i]->cancel_kind;
                    g_snap.state[i] = g_snap.member[i]->state;
                }
            }

            void expect_walk_done_at_first_switch()
            {
                ASSERT_TRUE(g_snap.taken) << "fixture: the walker never switched";
                for (int i = 0; i < g_snap.members; i++)
                {
                    EXPECT_EQ(g_snap.kind[i], CANCEL_SLAY)
                        << "member " << g_snap.member[i]->id << " unmarked at the first switch";
                    EXPECT_NE(g_snap.state[i], ThreadState::BLOCKED)
                        << "member " << g_snap.member[i]->id << " still parked at the first switch";
                }
                EXPECT_TRUE(g_snap.incoming_claimed)
                    << "member " << g_snap.incoming->id << " switched in without its claim";
            }

            Thread* creator_running()
            {
                Thread* const creator = seat_pool(0, PRIO_CREATOR);
                attach_caps(creator, KICKOS_SEAM_CAP_WIDTH);
                IrqLock lock;
                sched::reschedule(nullptr, lock);
                return creator;
            }

            Task* group_of(Thread* creator)
            {
                IrqLock lock;
                int err = 0;
                return task_held(kernel().threads.kill_tag_of(creator), &err, lock);
            }

            uint32_t g_word = 0;

            void park_on_word(Thread* t)
            {
                g_word = 0;
                park_word_waiter(t, plain_word(&g_word));
            }

            // A dying entry whose task serves the published console, with a sender parked on it
            // above the entry: mark_ended's refusal of that sender is a wake that outranks the
            // walker, the first switch an end can take.
            struct ConsoleEnd
            {
                Task* group;
                Thread* entry;
                Thread* sender;
            };

            ConsoleEnd console_end(int entry_slot, int sender_slot)
            {
                ConsoleEnd e{};
                Thread* const creator = creator_running();
                e.group = group_of(creator);
                Endpoint* const ep = endpoint();
                {
                    IrqLock lock;
                    int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
                    EXPECT_TRUE(cap_console_publish(creator, handle, lock));
                    cap_console_serve(e.group, lock);
                }
                e.entry = seat_pool(entry_slot, PRIO_WALKER);
                join_task(e.entry, e.group);
                e.entry->task_role = TASK_ROLE_ENTRY;
                e.sender = seat_pool(sender_slot, PRIO_WAKE);
                park_plain_sender(e.sender, ep);
                // Off the ready set, or the end's first pass takes the creator and not the sender.
                {
                    IrqLock lock;
                    detach_ready(creator);
                    seat_blocked(creator);
                }
                kernel().current(kickos_kernel_core()) = e.entry;
                return e;
            }

            void group_cancel_in_switch()
            {
                take_snapshot();
                IrqLock lock;
                task_cancel_group(g_group, lock);
                g_snap.left_state = g_left->state;
            }

            void drop_hold_in_switch()
            {
                take_snapshot();
                IrqLock lock;
                task_drop_hold(g_group, lock);
                g_snap.left_state = g_left->state;
            }
        }

        // Arm C: a dying entry's walk, the aborted member outranking it.
        TEST_F(SlayWalk, a_dying_entry_marks_every_member_before_its_first_switch)
        {
            Task* const group = task(0);
            Thread* const dying = seat_pool(0, PRIO_WALKER);
            Thread* const above = seat_pool(1, PRIO_WAKE);
            Thread* const later = seat_pool(2, PRIO_BELOW);
            join_task(dying, group);
            join_task(above, group);
            join_task(later, group);
            dying->task_role = TASK_ROLE_ENTRY;
            park_on_word(above);
            kernel().current(kickos_kernel_core()) = dying;
            Thread* const members[] = {above, later};
            watch(group, members, 2);
            run_at_next_switch(take_snapshot);

            run_exit(0);

            expect_walk_done_at_first_switch();
            EXPECT_EQ(g_snap.incoming, above) << "fixture: the pass took another thread";
        }

        // The end's own wakes come before the walk: every member is marked ahead of them.
        TEST_F(SlayWalk, an_end_s_first_wake_finds_every_member_marked)
        {
            ConsoleEnd const e = console_end(1, 3);
            Thread* const later = seat_pool(2, PRIO_BELOW);
            join_task(later, e.group);
            Thread* const members[] = {later};
            watch(e.group, members, 1);
            run_at_next_switch(take_snapshot);

            run_exit(0);

            ASSERT_TRUE(g_snap.taken);
            EXPECT_EQ(g_snap.incoming, e.sender) << "fixture: the first switch is not the refusal";
            EXPECT_EQ(g_snap.kind[0], CANCEL_SLAY) << "a member is unmarked at the end's first wake";
        }

        // A walker switched out between its marks and its aborts may not run again soon: any
        // later walk ends the parks of members marked before it, not only of those it marks.
        TEST_F(SlayWalk, a_later_walk_ends_the_parks_an_earlier_walker_left)
        {
            ConsoleEnd const e = console_end(1, 3);
            Thread* const parked = seat_pool(2, PRIO_BELOW);
            join_task(parked, e.group);
            park_on_word(parked);
            Thread* const members[] = {parked};
            watch(e.group, members, 1);
            g_left = parked;
            run_at_next_switch(group_cancel_in_switch);

            run_exit(0);

            ASSERT_TRUE(g_snap.taken);
            ASSERT_EQ(g_snap.kind[0], CANCEL_SLAY) << "fixture: the end's first wake came first";
            ASSERT_EQ(g_snap.state[0], ThreadState::BLOCKED) << "fixture: the end walked already";
            EXPECT_NE(g_snap.left_state, ThreadState::BLOCKED)
                << "a marked member stays parked behind a walker that has not resumed";
        }

        // The hold dropped while the walker is out, as a timeout-0 slayer slain before it runs
        // again drops it from its own exit: nothing else is left to end those parks.
        TEST_F(SlayWalk, a_dropped_hold_ends_the_parks_of_an_ended_task)
        {
            ConsoleEnd const e = console_end(1, 3);
            Thread* const parked = seat_pool(2, PRIO_BELOW);
            join_task(parked, e.group);
            park_on_word(parked);
            Thread* const members[] = {parked};
            watch(e.group, members, 1);
            g_left = parked;
            run_at_next_switch(drop_hold_in_switch);

            run_exit(0);

            ASSERT_TRUE(g_snap.taken);
            EXPECT_NE(g_snap.left_state, ThreadState::BLOCKED)
                << "the hold went with a member still parked in an ended task";
            EXPECT_EQ(parked->cancel_kind, CANCEL_SLAY);
        }

#if KICKOS_KERNEL_CORES > 1
        namespace
        {
            constexpr KernelCore CORE_PEER = core_at(1);
            Thread* g_on_peer;
            uint8_t g_peer_owed_at_switch;

            void read_peer_reseat()
            {
                take_snapshot();
                g_peer_owed_at_switch = g_on_peer->reseat_owed;
            }

            Thread* seat_peer_idle()
            {
                Thread* const t = &g_fx.t[0];
                t->base_prio = KICKOS_PRIO_IDLE;
                t->prio = KICKOS_PRIO_IDLE;
                t->id = 1;
                sched::add_idle(t, CORE_PEER);
                return t;
            }
        }

        // A member running on a peer core is asked off it before a RUNNING walker can be
        // switched out: the end's first wake may keep that walker off its core for long.
        TEST_F(SlayWalk, a_member_on_a_peer_is_asked_off_before_the_end_s_first_wake)
        {
            ConsoleEnd const e = console_end(1, 3);
            e.entry->affinity = 1u;
            e.sender->affinity = 1u;
            Thread* const running = seat_pool(2, PRIO_MEMBER);
            join_task(running, e.group);
            (void)seat_peer_idle();
            seat_running_on(running, CORE_PEER);
            seat_running_on(e.entry, 0);
            drain(0);
            drain(CORE_PEER);
            g_on_peer = running;
            g_peer_owed_at_switch = 0;
            Thread* const members[] = {running};
            watch(e.group, members, 1);
            run_at_next_switch(read_peer_reseat);

            run_exit(0);

            ASSERT_TRUE(g_snap.taken);
            EXPECT_EQ(g_snap.kind[0], CANCEL_SLAY);
            EXPECT_NE(g_peer_owed_at_switch, 0) << "the peer's member was not asked off yet";
        }
#endif
    }
}
