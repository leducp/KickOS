// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A wait's limit as the trap carries it, over the real time.cc and realtime.cc: the decode refuses
// a kind no wait takes, a deadline its clock has read is passed and never parks, no limit arms
// nothing, a realtime deadline moves with a set of the realtime clock, and a sleep for a relative
// 0 or a passed deadline yields without parking. Also kos_limit_word, the user side's encoding.

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
            constexpr uint64_t BASE_NS = 1700000000000000000ull;
            constexpr uint32_t UNKNOWN_KIND = 3u;

            uint32_t g_word = 0;
            Thread* g_setter = nullptr;
            uint64_t g_deadline_before = 0;
            uint64_t g_deadline_after = 0;

            uintptr_t word_va()
            {
                return reinterpret_cast<uintptr_t>(&g_word);
            }

            void set_realtime(uint64_t unix_ns)
            {
                ASSERT_EQ(realtime_set(g_setter, unix_ns), 0);
            }

            // The waiter is CURRENT and the holder, below it, the only other runnable thread, so a
            // park hands the CPU to the holder and a yield keeps it.
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
                g_now_ns = 5u * MS_NS;
                set_realtime(BASE_NS);
                g_switches = 0;
                return waiter;
            }

            void back_an_hour_then_expire(Thread* parked)
            {
                g_deadline_before = parked->deadline_ns;
                set_realtime(realtime_now() - HOUR_NS);
                g_deadline_after = parked->deadline_ns;
                g_now_ns = g_deadline_after;
                ktime_on_timer();
            }

            // Neither parked nor holding a deadline, and still the running thread.
            void expect_unparked(Thread const* waiter)
            {
                EXPECT_EQ(g_switches, 0u) << "a wait that may not park switched away";
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter);
                EXPECT_NE(waiter->state, ThreadState::BLOCKED);
                EXPECT_FALSE(waiter->on_timer);
                EXPECT_EQ(kernel().sleepq(kickos_kernel_core()), nullptr);
            }

            class KTimeLimit : public KSeam
            {
            };
        }

        TEST_F(KTimeLimit, the_decode_takes_the_two_clocks_and_refuses_every_other_kind)
        {
            EXPECT_EQ(ktime_limit_word(0x89abcdefu, 0x01234567u), 0x0123456789abcdefull);
            EXPECT_TRUE(ktime_limit_kind_ok(KOS_LIMIT_MONOTONIC));
            EXPECT_TRUE(ktime_limit_kind_ok(KOS_LIMIT_REALTIME));
            EXPECT_FALSE(ktime_limit_kind_ok(KOS_LIMIT_RELATIVE)) << "a relative limit is a sleep's alone";
            EXPECT_FALSE(ktime_limit_kind_ok(UNKNOWN_KIND));
        }

        TEST_F(KTimeLimit, a_deadline_is_passed_once_its_own_clock_reads_it)
        {
            seat_waiter_over_holder();
            EXPECT_TRUE(ktime_limit_passed(WaitLimit(0u)));
            EXPECT_TRUE(ktime_limit_passed(WaitLimit(g_now_ns)));
            EXPECT_FALSE(ktime_limit_passed(WaitLimit(g_now_ns + 1u)));
            EXPECT_FALSE(ktime_limit_passed(WaitLimit(KOS_LIMIT_NONE)));
            EXPECT_TRUE(ktime_limit_passed(WaitLimit(realtime_now(), KOS_LIMIT_REALTIME)));
            EXPECT_FALSE(ktime_limit_passed(WaitLimit(realtime_now() + 1u, KOS_LIMIT_REALTIME)));
            EXPECT_FALSE(ktime_limit_passed(WaitLimit(KOS_LIMIT_NONE, KOS_LIMIT_REALTIME)));
            EXPECT_TRUE(ktime_limit_passed(WaitLimit(g_now_ns + 1u, KOS_LIMIT_REALTIME)))
                << "a realtime deadline was read against the monotonic clock";
            EXPECT_FALSE(ktime_limit_passed(WaitLimit(realtime_now())))
                << "a monotonic deadline was read against the realtime clock";
        }

        // No waker is armed: a park here ends the arm with the fixture's refusal.
        TEST_F(KTimeLimit, a_wait_given_a_passed_deadline_never_parks)
        {
            Thread* const waiter = seat_waiter_over_holder();
            EXPECT_EQ(word_wait(word_va(), g_word, g_now_ns), -KOS_ETIMEDOUT);
            EXPECT_EQ(word_wait(word_va(), g_word, BASE_NS, KOS_LIMIT_REALTIME),
                      -KOS_ETIMEDOUT);
            expect_unparked(waiter);
        }

        TEST_F(KTimeLimit, no_limit_arms_nothing_and_a_deadline_arms_floored)
        {
            Thread* const waiter = seat_waiter_over_holder();
            {
                IrqLock lock;
                ktime_limit_arm(waiter, WaitLimit(KOS_LIMIT_NONE), lock);
                ktime_limit_arm(waiter, WaitLimit(KOS_LIMIT_NONE, KOS_LIMIT_REALTIME), lock);
            }
            EXPECT_FALSE(waiter->on_timer) << "no limit put the thread on the sleep queue";
            EXPECT_EQ(kernel().sleepq(kickos_kernel_core()), nullptr);

            {
                IrqLock lock;
                ktime_limit_arm(waiter, WaitLimit(g_now_ns + 1u), lock);
            }
            EXPECT_TRUE(waiter->on_timer);
            EXPECT_EQ(kernel().sleepq(kickos_kernel_core()), waiter);
            EXPECT_EQ(waiter->deadline_ns, g_now_ns + KICKOS_TIMER_MIN_DELTA_NS)
                << "a deadline inside the timer's floor was armed closer than it";
            IrqLock lock;
            ktime_deadline_cancel(waiter, lock);
        }

        TEST_F(KTimeLimit, a_realtime_deadline_moves_with_a_set_of_the_realtime_clock)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(back_an_hour_then_expire);

            EXPECT_EQ(ktime_sleep(BASE_NS + MS_NS, KOS_LIMIT_REALTIME), 0);

            EXPECT_EQ(g_deadline_before, 6u * MS_NS)
                << "the deadline is not the realtime left on the monotonic clock";
            EXPECT_EQ(g_deadline_after, g_deadline_before + HOUR_NS)
                << "a set an hour back did not move the realtime deadline an hour out";
            EXPECT_EQ(waiter->wait_result, -KOS_ETIMEDOUT);
            EXPECT_FALSE(waiter->on_timer);
            EXPECT_EQ(kernel().sleepq(kickos_kernel_core()), nullptr);
        }

        // No waker is armed: a park here ends the arm with the fixture's refusal.
        TEST_F(KTimeLimit, a_sleep_for_a_relative_zero_or_a_passed_deadline_yields_without_parking)
        {
            Thread* const waiter = seat_waiter_over_holder();
            EXPECT_EQ(ktime_sleep(0u, KOS_LIMIT_RELATIVE), 0);
            EXPECT_EQ(ktime_sleep(g_now_ns, KOS_LIMIT_MONOTONIC), 0);
            EXPECT_EQ(ktime_sleep(0u, KOS_LIMIT_MONOTONIC), 0);
            EXPECT_EQ(ktime_sleep(BASE_NS, KOS_LIMIT_REALTIME), 0);
            expect_unparked(waiter);
        }

        TEST_F(KTimeLimit, a_sleep_of_an_unknown_kind_is_refused_and_does_not_park)
        {
            Thread* const waiter = seat_waiter_over_holder();
            EXPECT_EQ(ktime_sleep(g_now_ns + MS_NS, UNKNOWN_KIND), -KOS_EINVAL);
            expect_unparked(waiter);
        }

        TEST(KosLimitWord, a_negative_count_is_a_poll_and_the_largest_no_limit)
        {
            EXPECT_EQ(kos_limit_word(-1), 0u);
            EXPECT_EQ(kos_limit_word(INT64_MIN), 0u);
            EXPECT_EQ(kos_limit_word(0), 0u);
            EXPECT_EQ(kos_limit_word(5), 5u);
            EXPECT_EQ(kos_limit_word(INT64_MAX - 1), static_cast<uint64_t>(INT64_MAX - 1));
            EXPECT_EQ(kos_limit_word(INT64_MAX), KOS_LIMIT_NONE);
            EXPECT_EQ(kos_limit_word(KOS_NEVER.ns), KOS_LIMIT_NONE);
            EXPECT_EQ(kos_limit_word(KOS_FOREVER.ns), KOS_LIMIT_NONE);
            EXPECT_EQ(kos_limit_word(KOS_POLL.ns), 0u);
        }
    }
}
