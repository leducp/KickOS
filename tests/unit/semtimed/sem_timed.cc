// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The timed semaphore wait over the real sync.cc, park.cc and time.cc: what the waiter answers
// and where the token ends up when its deadline, a post and a cancel reach the park. The post and
// the expiry are SEQUENCED from the park's own switch, in both orders, the post coming from the
// peer core above one kernel core; nothing here runs them at once.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include <string>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class SemTimed : public KSeam
            {
            };

            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;
            constexpr uint8_t PRIO_BELOW = 3;
            constexpr uint8_t PRIO_ABOVE = 7;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr int SLOT_FIRST = 2;
            constexpr int SLOT_LAST = 3;
            constexpr uint32_t TIMEOUT_US = 1000;
#if KICKOS_KERNEL_CORES > 1
            constexpr uint32_t CORE_POSTER = KICKOS_KERNEL_CORES - 1;
#endif

            Semaphore* g_sem = nullptr;

            // A post from the core a peer thread runs on.
            void post_from_peer()
            {
#if KICKOS_KERNEL_CORES > 1
                uint32_t const was = g_core;
                g_core = CORE_POSTER;
                (void)sem_post(g_sem);
                g_core = was;
#else
                (void)sem_post(g_sem);
#endif
            }

            // The clock reaches the deadline and its interrupt is taken.
            void deadline_fires()
            {
                g_now_ns += static_cast<uint64_t>(TIMEOUT_US) * 1000u + KICKOS_TIMER_MIN_DELTA_NS;
                ktime_on_timer();
            }

            void post_then_expire(Thread*)
            {
                post_from_peer();
                deadline_fires();
            }

            void expire_then_post(Thread*)
            {
                deadline_fires();
                post_from_peer();
            }

            void cancel(Thread* parked)
            {
                thread_cancel(parked, kickos::IrqLock());
            }

            void cancel_then_expire(Thread* parked)
            {
                cancel(parked);
                deadline_fires();
            }

            // KOS_SYS_SEM_WAIT's shape: the park under the resolve's bracket, the result read
            // after it, past the resume barrier.
            int timed_wait(uint32_t timeout_us)
            {
                Thread* const c = sched::current();
                uint32_t epoch = 0;
                {
                    IrqLock lock;
                    SemWait const got = sem_wait(lock, g_sem, timeout_us, epoch);
                    if (got == SemWait::TAKEN)
                    {
                        return 0;
                    }
                    if (got == SemWait::EMPTY)
                    {
                        return -KOS_ETIMEDOUT;
                    }
                }
                wq_confirm_resume(c, epoch);
                return static_cast<int>(c->wait_result);
            }

            // The waiter is CURRENT, and the holder the only other runnable thread, so the
            // park hands the CPU to the holder and the waker runs as it.
            Thread* seat_waiter_over_holder()
            {
                (void)seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter)
                    << "fixture: the waiter is current";
                g_switches = 0;
                g_sem = semaphore(nullptr);
                return waiter;
            }

            bool sem_has_waiters()
            {
                return g_sem->waiters.head != nullptr;
            }

            void slay_then_expire(Thread* parked)
            {
                thread_cancel_kind(parked, CANCEL_SLAY, kickos::IrqLock());
                deadline_fires();
            }

            void slay(Thread* parked)
            {
                thread_cancel_kind(parked, CANCEL_SLAY, kickos::IrqLock());
            }

            // A waiter parked by hand behind the one the arm parks, at a priority the post
            // decides by.
            Thread* g_last = nullptr;

            void queue_last_then_expire(Thread*)
            {
                g_last = seat_pool(SLOT_LAST, PRIO_BELOW);
                park_sem_waiter(g_last, g_sem);
                deadline_fires();
            }

            Thread* g_above = nullptr;

            void queue_above_post_then_expire(Thread*)
            {
                g_above = seat_pool(SLOT_FIRST, PRIO_ABOVE);
                park_sem_waiter(g_above, g_sem);
                post_from_peer();
                deadline_fires();
            }

            Thread* g_sleepq_while_parked = nullptr;
            bool g_on_timer_while_parked = false;

            void note_the_timer_then_post(Thread* parked)
            {
                g_sleepq_while_parked = kernel().sleepq;
                g_on_timer_while_parked = parked->on_timer;
                post_from_peer();
            }

            void try_while_cancelled()
            {
                (void)timed_wait(0);
            }

            bool queue_is(Thread const* first, Thread const* second)
            {
                ListNode const* const h = g_sem->waiters.head;
                return h == &first->link and h->next == &second->link and h->next->next == nullptr
                       and g_sem->waiters.tail == &second->link;
            }

            struct Waker
            {
                char const* name;
                ParkWaker waker;
                uint32_t timeout_us;
                int rc;
                int count;
                uint8_t kind;
            };

            class SemWaker : public KSeam, public ::testing::WithParamInterface<Waker>
            {
            };
        }

        TEST_P(SemWaker, answers_its_waker_and_leaves_no_waiter_or_deadline)
        {
            Waker const& w = GetParam();
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(w.waker);

            EXPECT_EQ(timed_wait(w.timeout_us), w.rc);
            EXPECT_EQ(g_sem->count, w.count);
            EXPECT_EQ(waiter->cancel_kind, w.kind);
            EXPECT_FALSE(sem_has_waiters()) << "the waiter was left on the queue";
            EXPECT_EQ(kernel().sleepq, nullptr) << "the waiter's deadline is still queued";
        }

        INSTANTIATE_TEST_SUITE_P(
            Park, SemWaker,
            ::testing::Values(
                Waker{"a_post_landing_first_at_the_deadline_hands_its_token", post_then_expire, TIMEOUT_US, 0, 0,
                      CANCEL_NONE},
                Waker{"an_expiry_landing_first_leaves_the_post_banked", expire_then_post, TIMEOUT_US, -KOS_ETIMEDOUT,
                      1, CANCEL_NONE},
                Waker{"a_cancel_answers_ecanceled_before_the_deadline", cancel_then_expire, TIMEOUT_US,
                      -KOS_ECANCELED, 0, CANCEL_KILL},
                Waker{"a_slay_answers_ecanceled_before_the_deadline", slay_then_expire, TIMEOUT_US, -KOS_ECANCELED,
                      0, CANCEL_SLAY},
                Waker{"an_untimed_cancel_answers_ecanceled", cancel, KOS_TIMEOUT_NONE, -KOS_ECANCELED, 0, CANCEL_KILL}),
            [](::testing::TestParamInfo<Waker> const& p)
            {
                return std::string{p.param.name};
            });

        TEST_F(SemTimed, a_zero_timeout_takes_a_banked_token_or_answers_at_once)
        {
            seat_waiter_over_holder();

            // No waker is armed: a park here ends the arm with the fixture's refusal.
            EXPECT_EQ(timed_wait(0), -KOS_ETIMEDOUT) << "nothing banked, so a try fails";
            EXPECT_EQ(g_sem->count, 0);
            EXPECT_EQ(g_switches, 0u) << "a try switched away";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a try armed a deadline";

            post_from_peer();
            EXPECT_EQ(timed_wait(0), 0) << "a banked token is taken";
            post_from_peer();
            EXPECT_EQ(timed_wait(TIMEOUT_US), 0) << "a banked token is taken by a timed wait";
            EXPECT_EQ(g_sem->count, 0);
            EXPECT_EQ(g_switches, 0u);
            EXPECT_EQ(kernel().sleepq, nullptr) << "a wait that never parked left a deadline";
        }

        // The slot a slain timed waiter leaves is the next thread's: a deadline left queued on
        // it would fire into a thread that never waited.
        TEST_F(SemTimed, a_slot_reused_after_a_slain_timed_waiter_carries_no_deadline)
        {
            seat_waiter_over_holder();
            wake_next_park(slay);
            ASSERT_EQ(timed_wait(TIMEOUT_US), -KOS_ECANCELED);
            run_exit(KOS_EXIT_CANCELLED);

            Thread* const next = seat_pool(SLOT_WAITER, PRIO_BELOW);
            EXPECT_EQ(kernel().sleepq, nullptr)
                << "the slain waiter's deadline outlived it and now names its slot's next thread";
            EXPECT_FALSE(next->on_timer);
            next->wait_result = WAIT_RESULT_POISON;
            deadline_fires();
            EXPECT_EQ(next->wait_result, WAIT_RESULT_POISON)
                << "the old deadline fired into a thread that never waited";
        }

        TEST_F(SemTimed, an_expired_waiter_in_the_middle_is_unlinked_alone)
        {
            seat_waiter_over_holder();
            Thread* const first = seat_pool(SLOT_FIRST, PRIO_BELOW);
            park_sem_waiter(first, g_sem);
            wake_next_park(queue_last_then_expire);

            int const rc = timed_wait(TIMEOUT_US);

            EXPECT_EQ(rc, -KOS_ETIMEDOUT);
            EXPECT_TRUE(queue_is(first, g_last))
                << "the expiry unlinked some other waiter than the one whose deadline fired";
            EXPECT_EQ(g_sem->count, 0);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(SemTimed, a_post_goes_to_the_highest_waiter_with_a_timed_one_queued_first)
        {
            seat_waiter_over_holder();
            wake_next_park(queue_above_post_then_expire);

            int const rc = timed_wait(TIMEOUT_US);

            EXPECT_EQ(g_above->wait_result, 0) << "the post did not go to the highest waiter";
            EXPECT_NE(g_above->state, ThreadState::BLOCKED);
            EXPECT_EQ(rc, -KOS_ETIMEDOUT) << "the timed waiter queued first took the token";
            EXPECT_FALSE(sem_has_waiters());
            EXPECT_EQ(g_sem->count, 0);
        }

        TEST_F(SemTimed, an_untimed_wait_never_reaches_the_timer)
        {
            seat_waiter_over_holder();
            wake_next_park(note_the_timer_then_post);

            int const rc = timed_wait(KOS_TIMEOUT_NONE);

            EXPECT_EQ(rc, 0);
            EXPECT_EQ(g_sem->count, 0) << "the token went to the waiter, not to the count";
            EXPECT_FALSE(sem_has_waiters());
            EXPECT_EQ(g_sleepq_while_parked, nullptr) << "an untimed park queued a deadline";
            EXPECT_FALSE(g_on_timer_while_parked);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        // A cancel pending at entry is the death point whatever the timeout, a try included.
        TEST_F(SemTimed, a_pending_cancel_ends_a_try_before_it_answers)
        {
            Thread* const waiter = seat_waiter_over_holder();
            {
                IrqLock lock;
                thread_cancel(waiter, lock);
            }

            run_noreturn(try_while_cancelled);

            EXPECT_EQ(waiter->state, ThreadState::EXITED) << "the try answered a cancelled thread";
            EXPECT_EQ(g_sem->count, 0);
        }
    }
}
