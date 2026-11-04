// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A wait toward a realtime deadline over the real sync.cc, park.cc, time.cc and realtime.cc: a set
// of the wall clock while it is parked moves its deadline by the set's change, and leaves a
// relative wait's alone; and the delta list stays sorted with several of each queued.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include <vector>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr uint64_t MS_NS = 1000000u;
            constexpr uint64_t HOUR_NS = 3600000000000ull;
            constexpr uint64_t DAY_NS = 24u * HOUR_NS;
            constexpr uint64_t BASE_NS = 1700000000000000000ull;

            uint32_t g_word = 0;
            Thread* g_setter = nullptr;

            uintptr_t word_va()
            {
                return reinterpret_cast<uintptr_t>(&g_word);
            }

            void set_wall(uint64_t unix_ns)
            {
                ASSERT_EQ(realtime_set(g_setter, unix_ns), 0);
            }

            // The clock reaches `ns` from now and its interrupt is taken.
            void advance(uint64_t ns)
            {
                g_now_ns += ns;
                ktime_on_timer();
            }

            // The waiter is CURRENT and the holder the only other runnable thread, so the park
            // hands the CPU to the holder, which sets the clock.
            Thread* seat_waiter_over_holder()
            {
                g_setter = seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                g_setter->authority = AUTH_REALTIME;
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter)
                    << "fixture: the waiter is current";
                g_word = KOS_WORD_WAITERS;
                set_wall(BASE_NS);
                return waiter;
            }

            int wait_wall(uint64_t unix_ns)
            {
                return word_wait(word_va(), g_word, WaitLimit::realtime(unix_ns));
            }

            int wait_relative(uint32_t timeout_us)
            {
                return word_wait(word_va(), g_word, timeout_us);
            }

            // What each parked waiter saw of itself while the arm's waker ran.
            uint64_t g_deadline_before = 0;
            uint64_t g_deadline_after = 0;
            bool g_parked_past_deadline = false;

            void forward_a_day_past_the_deadline(Thread* parked)
            {
                g_deadline_before = parked->deadline_ns;
                set_wall(realtime_now() + DAY_NS);
                g_deadline_after = parked->deadline_ns;
                advance(KICKOS_TIMER_MIN_DELTA_NS);
            }

            void back_an_hour_then_wait_it_out(Thread* parked)
            {
                g_deadline_before = parked->deadline_ns;
                set_wall(realtime_now() - HOUR_NS);
                g_deadline_after = parked->deadline_ns;
                advance(MS_NS + KICKOS_TIMER_MIN_DELTA_NS);
                g_parked_past_deadline = parked->state == ThreadState::BLOCKED;
                advance(HOUR_NS);
            }

            void forward_then_back_then_its_own_deadline(Thread* parked)
            {
                g_deadline_before = parked->deadline_ns;
                set_wall(realtime_now() + DAY_NS);
                set_wall(realtime_now() - 2u * DAY_NS);
                g_deadline_after = parked->deadline_ns;
                advance(KICKOS_TIMER_MIN_DELTA_NS);
                g_parked_past_deadline = parked->state == ThreadState::BLOCKED;
                advance(MS_NS);
            }

            class WordWall : public KSeam
            {
            };
        }

        TEST_F(WordWall, a_forward_set_past_a_realtime_deadline_ends_the_wait_at_once)
        {
            seat_waiter_over_holder();
            uint64_t const now = g_now_ns;
            wake_next_park(forward_a_day_past_the_deadline);

            EXPECT_EQ(wait_wall(BASE_NS + HOUR_NS), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_deadline_before, now + HOUR_NS)
                << "the deadline is not the wall time left on the monotonic clock";
            EXPECT_EQ(g_deadline_after, now + KICKOS_TIMER_MIN_DELTA_NS)
                << "the set did not move the passed deadline to the soonest the timer takes";
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(WordWall, a_backward_set_pushes_a_realtime_deadline_out_by_the_set)
        {
            seat_waiter_over_holder();
            wake_next_park(back_an_hour_then_wait_it_out);

            EXPECT_EQ(wait_wall(BASE_NS + MS_NS), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_deadline_after, g_deadline_before + HOUR_NS);
            EXPECT_TRUE(g_parked_past_deadline)
                << "the wait ended at the deadline the set moved an hour out";
        }

        TEST_F(WordWall, a_relative_wait_keeps_its_deadline_across_both_sets)
        {
            seat_waiter_over_holder();
            wake_next_park(forward_then_back_then_its_own_deadline);

            EXPECT_EQ(wait_relative(1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_deadline_after, g_deadline_before) << "a set moved a relative deadline";
            EXPECT_TRUE(g_parked_past_deadline) << "a forward set ended a relative wait";
        }

        TEST_F(WordWall, a_passed_realtime_deadline_answers_at_once_with_nothing_parked)
        {
            seat_waiter_over_holder();
            g_switches = 0;

            // No waker is armed: a park here ends the arm with the fixture's refusal.
            EXPECT_EQ(wait_wall(BASE_NS), -KOS_ETIMEDOUT);
            EXPECT_EQ(wait_wall(0u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_switches, 0u) << "a passed deadline switched away";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a passed deadline armed one";
        }

        TEST_F(WordWall, a_limit_the_abi_does_not_define_is_refused)
        {
            seat_waiter_over_holder();
            EXPECT_EQ(word_wait(word_va(), g_word, WaitLimit(1000u, 1u)), -KOS_EINVAL);
            EXPECT_EQ(mutex_lock(word_va(), WaitLimit(1000u, 0x40000000u)), -KOS_EINVAL);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        // Marked deadlines among relative ones: each set moves the marked ones alone, by its
        // change, and the list stays sorted, equals in the order they were queued.
        TEST_F(WordWall, several_realtime_waiters_are_rekeyed_in_order_among_relative_ones)
        {
            seat_waiter_over_holder();
            uint64_t const now = g_now_ns;
            WordRecord* const m = plain_word(&g_word);
            struct Queued
            {
                Thread* t;
                bool wall;
                uint64_t in_ns;
            };
            std::vector<Queued> q = {
                {nullptr, true, 10u * MS_NS},  {nullptr, false, 15u * MS_NS},
                {nullptr, true, 20u * MS_NS},  {nullptr, false, 25u * MS_NS},
                {nullptr, true, 30u * MS_NS},  {nullptr, true, 30u * MS_NS},
            };
            for (size_t i = 0; i < q.size(); i++)
            {
                q[i].t = seat_pool(static_cast<int>(2u + i), PRIO_HOLDER);
                park_word_waiter(q[i].t, m);
                IrqLock lock;
                if (q[i].wall)
                {
                    realtime_deadline_arm(q[i].t, BASE_NS + q[i].in_ns, lock);
                }
                else
                {
                    ktime_deadline_arm(q[i].t, static_cast<uint32_t>(q[i].in_ns / 1000u), lock);
                }
            }
            auto order = []() {
                std::vector<Thread*> v;
                for (Thread* t = kernel().sleepq; t != nullptr; t = t->tnext)
                {
                    v.push_back(t);
                }
                return v;
            };
            auto sorted = []() {
                for (Thread* t = kernel().sleepq; t != nullptr and t->tnext != nullptr;
                     t = t->tnext)
                {
                    if (t->deadline_ns > t->tnext->deadline_ns)
                    {
                        return false;
                    }
                }
                return true;
            };
            ASSERT_EQ(order(), (std::vector<Thread*>{q[0].t, q[1].t, q[2].t, q[3].t, q[4].t,
                                                     q[5].t}));

            set_wall(BASE_NS + 12u * MS_NS);

            EXPECT_TRUE(sorted());
            EXPECT_EQ(order(), (std::vector<Thread*>{q[0].t, q[2].t, q[1].t, q[4].t, q[5].t,
                                                     q[3].t}));
            EXPECT_EQ(q[0].t->deadline_ns, now + KICKOS_TIMER_MIN_DELTA_NS);
            EXPECT_EQ(q[1].t->deadline_ns, now + 15u * MS_NS) << "a relative deadline moved";
            EXPECT_EQ(q[2].t->deadline_ns, now + 8u * MS_NS);
            EXPECT_EQ(q[4].t->deadline_ns, now + 18u * MS_NS);

            set_wall(BASE_NS + 12u * MS_NS - HOUR_NS);

            EXPECT_TRUE(sorted());
            EXPECT_EQ(order(), (std::vector<Thread*>{q[1].t, q[3].t, q[0].t, q[2].t, q[4].t,
                                                     q[5].t}));
            EXPECT_EQ(q[2].t->deadline_ns, now + 8u * MS_NS + HOUR_NS);
            EXPECT_EQ(q[3].t->deadline_ns, now + 25u * MS_NS) << "a relative deadline moved";

            advance(HOUR_NS + 30u * MS_NS);
            EXPECT_EQ(kernel().sleepq, nullptr) << "a deadline outlived every set";
            for (Queued const& e : q)
            {
                EXPECT_EQ(e.t->wait_result, -KOS_ETIMEDOUT);
            }
        }
    }
}
