// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// word_reach_lost on a backend whose switch is taken where it is booked: a waiter it ends that
// outranks the caller must not switch away before the walk is over, or the walk resumes on list
// pointers other threads have moved meanwhile.

#include <kickos/domain.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h>
#include <kickos/vrange.h>

#include <kickos/sys/errno.h>

#include "aspace_seam.h"
#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            Thread* g_late = nullptr;
            ThreadState g_late_state_at_switch = ThreadState::INACTIVE;

            class ReachLost : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    aspace_seam_reset();
                    g_late = nullptr;
                    g_late_state_at_switch = ThreadState::INACTIVE;
                }
            };

            static_assert(KICKOS_HAVE_ASPACE, "word_reach_lost is a translating kernel's call");

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;
            constexpr uint8_t PRIO_HIGH = 6;
            constexpr uint32_t TIMEOUT_US = 1000;
            constexpr size_t PAGE = 4096;
            constexpr uint16_t HOLDER = 2;

            Thread* member_of(int slot, uint8_t prio, Task* group)
            {
                Thread* const t = seat_pool(slot, prio);
                join_task(t, group);
                return t;
            }

            void run_as(Thread* t)
            {
                kernel().current(testfix::core_at(0)) = t;
                t->state.to<ThreadState::RUNNING>();
                install_for(t);
            }

            uintptr_t va(uint32_t const* word)
            {
                return reinterpret_cast<uintptr_t>(word);
            }

            void reach_lost(Thread const* c, uint32_t const* word)
            {
                IrqLock lock;
                word_reach_lost(domain_space(thread_domain(c)), va(word), sizeof(*word), lock);
            }

            void note_late_state()
            {
                g_late_state_at_switch = g_late->state;
            }

            // What the fixture's timer stands in for: an expiry is a deadline still armed.
            void late_waiter_times_out()
            {
                if (g_late->on_timer)
                {
                    IrqLock lock;
                    thread_abort_park(g_late, -KOS_ETIMEDOUT, lock);
                }
            }
        }

        TEST_F(ReachLost, every_wait_in_the_range_ends_before_the_scan_switches)
        {
            Task* const group = task(0);
            Thread* const caller = member_of(1, PRIO_LOW, group);
            Thread* const first = member_of(2, PRIO_HIGH, group);
            g_late = member_of(3, PRIO_HIGH, group);
            uint32_t word = 0;
            WordRecord* const m = plain_word(&word);
            park_word_waiter(first, m);
            park_word_waiter(g_late, m);
            run_as(caller);
            run_at_next_switch(note_late_state);

            reach_lost(caller, &word);

            EXPECT_EQ(g_switches, 1u);
            EXPECT_NE(g_late_state_at_switch, ThreadState::BLOCKED)
                << "the scan switched away with a waiter of the range still parked behind it";
            EXPECT_EQ(first->wait_result, -KOS_EFAULT);
            EXPECT_EQ(g_late->wait_result, -KOS_EFAULT);
            EXPECT_EQ(kernel().word_active, nullptr);
        }

        TEST_F(ReachLost, a_timeout_at_the_scans_switch_finds_the_wait_already_ended)
        {
            Task* const group = task(0);
            Thread* const caller = member_of(1, PRIO_LOW, group);
            Thread* const first = member_of(2, PRIO_HIGH, group);
            g_late = member_of(3, PRIO_HIGH, group);
            uint32_t word = 0;
            WordRecord* const m = plain_word(&word);
            park_word_waiter(first, m);
            park_word_waiter(g_late, m);
            {
                IrqLock lock;
                ktime_deadline_arm(g_late, TIMEOUT_US, lock);
            }
            run_as(caller);
            run_at_next_switch(late_waiter_times_out);

            reach_lost(caller, &word);

            EXPECT_EQ(first->wait_result, -KOS_EFAULT);
            EXPECT_EQ(g_late->wait_result, -KOS_EFAULT);
            EXPECT_EQ(g_late->wait_kind, WAIT_NONE);
            EXPECT_EQ(kernel().word_active, nullptr);
        }

        TEST_F(ReachLost, a_death_ends_the_waits_of_every_window_before_it_switches)
        {
            alignas(PAGE) static uint32_t pages[2][PAGE / sizeof(uint32_t)];
            Task* const group = task(0);
            Thread* const caller = member_of(1, PRIO_LOW, group);
            Thread* const first = member_of(2, PRIO_HIGH, group);
            g_late = member_of(3, PRIO_HIGH, group);
            VirtualRanges windows;
            ASSERT_TRUE(windows.init(PAGE));
            ASSERT_TRUE(windows.reserve(va(pages[0]), 1, VR_BORROWED | VR_WINDOW, 0, HOLDER));
            ASSERT_TRUE(windows.reserve(va(pages[1]), 1, VR_BORROWED | VR_WINDOW, 0, HOLDER));
            park_word_waiter(first, plain_word(&pages[0][0]));
            park_word_waiter(g_late, plain_word(&pages[1][0]));
            run_as(caller);
            run_at_next_switch(note_late_state);
            {
                IrqLock lock;
                word_windows_lost(domain_space(thread_domain(caller)), &windows, HOLDER, lock);
            }

            EXPECT_EQ(g_switches, 1u);
            EXPECT_NE(g_late_state_at_switch, ThreadState::BLOCKED)
                << "a window's waits were still parked across another window's switch";
            EXPECT_EQ(first->wait_result, -KOS_EFAULT);
            EXPECT_EQ(g_late->wait_result, -KOS_EFAULT);
            EXPECT_EQ(kernel().word_active, nullptr);
        }

        // An exiting caller declines a pass for a woken thread it outranks, so the pass must be
        // weighed on the highest thread the walk woke, not the first.
        TEST_F(ReachLost, a_dying_caller_yields_to_the_highest_waiter_the_walk_woke)
        {
            Task* const group = task(0);
            Thread* const caller = member_of(1, PRIO_MID, group);
            Thread* const low = member_of(2, PRIO_LOW, group);
            Thread* const high = member_of(3, PRIO_HIGH, group);
            uint32_t word = 0;
            WordRecord* const m = plain_word(&word);
            park_word_waiter(low, m);
            park_word_waiter(high, m);
            run_as(caller);
            caller->dying = true;

            reach_lost(caller, &word);

            EXPECT_EQ(g_switches, 1u);
            EXPECT_EQ(kernel().current(testfix::core_at(0)), high);
            EXPECT_EQ(low->wait_result, -KOS_EFAULT);
            EXPECT_EQ(high->wait_result, -KOS_EFAULT);
        }
    }
}
