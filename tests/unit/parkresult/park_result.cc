// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a blocking primitive RETURNS, driven for real rather than seated by hand. The park
// STATE was always assertable on this fixture; the return value was not, because arch_switch
// returns and hands the CPU straight back to the thread that parked, with no waker having
// run. kfixture.h note 2 is the mechanism.
//
// The primitives reachable here are sync.cc's, because that is the translation unit the
// K-seam compiles. endpoint_recv lives in syscall_ipc.cc and is out of the gate's reach.

#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class ParkResult : public KSeam
            {
            };

            // gtest runs a *DeathTest suite ahead of the others, which is the documented
            // placement for a forking case.
            class ParkResultDeathTest : public KSeam
            {
            };

            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;

            // The waker reads them because a ParkWaker takes only the parked thread: it
            // stands in for a peer, and a peer holds its own object references.
            Mutex* g_mutex = nullptr;
            Semaphore* g_sem = nullptr;

            void unlock_the_mutex(Thread*)
            {
                (void) mutex_unlock(g_mutex);
            }

            void cancel_the_waiter(Thread* parked)
            {
                thread_cancel(parked);
            }

            void post_the_semaphore(Thread*)
            {
                (void) sem_post(g_sem);
            }

            // Hand-rolled: every real waker in the tree writes a result; this one deliberately
            // does not, to exercise note 2's poison.
            void end_the_park_writing_nothing(Thread* parked)
            {
                parked->wait_queue->unlink(&parked->link);
                parked->clear_wait_edge();
                sched::wake(parked);
            }

            // The waiter is CURRENT, because a blocking primitive parks whoever calls it,
            // and the holder is the only other runnable thread so the scheduler must pick
            // it -- which is what puts the waker's `current` on the holder.
            Thread* seat_waiter_over_holder(Thread** out_holder)
            {
                Thread* const holder = spawn(0, PRIO_HOLDER);
                Thread* const waiter = spawn(1, PRIO_WAITER);
                sched::reschedule();
                EXPECT_EQ(kernel().current, waiter) << "fixture: the waiter is current";
                g_switches = 0;
                trace_reset();
                *out_holder = holder;
                return waiter;
            }
        }

        // --- the four arms that supply a waker ----------------------------------------

        TEST_F(ParkResult, a_woken_mutex_lock_returns_what_the_waker_wrote)
        {
            Thread* holder = nullptr;
            Thread* const waiter = seat_waiter_over_holder(&holder);
            int handle = 0;
            g_mutex = own_mutex(holder, &handle);
            wake_next_park(unlock_the_mutex);

            int const rc = mutex_lock(g_mutex);

            EXPECT_EQ(rc, 0) << "the transfer said the lock was acquired";
            EXPECT_EQ(g_mutex->owner, waiter) << "ownership moved to the woken waiter";
            // The second switch is the waker's own: the new owner outranks the holder it
            // just reverted, so mutex_unlock's wake takes the CPU back.
            EXPECT_STREQ(trace(), "switch2>1 switch1>2") << "the park switched away and back";
            EXPECT_EQ(holder->prio, PRIO_HOLDER) << "the holder's donated boost was reverted";
        }

        TEST_F(ParkResult, a_cancelled_mutex_lock_returns_ecanceled)
        {
            Thread* holder = nullptr;
            Thread* const waiter = seat_waiter_over_holder(&holder);
            int handle = 0;
            g_mutex = own_mutex(holder, &handle);
            wake_next_park(cancel_the_waiter);

            int const rc = mutex_lock(g_mutex);

            EXPECT_EQ(rc, -KOS_ECANCELED) << "the cancel is what the return carries";
            EXPECT_TRUE(waiter->cancelled) << "and the waiter is marked for its death point";
            EXPECT_EQ(g_mutex->owner, holder) << "a cancelled waiter does not acquire the mutex";
            EXPECT_EQ(holder->prio, PRIO_HOLDER) << "the owner's boost is reverted with the wait";
        }

        // A semaphore park has no error channel at all: sem_wait returns void and reads no
        // wait_result. The mechanism still demands its waker, because the park is real.
        TEST_F(ParkResult, a_posted_semaphore_park_hands_over_its_token)
        {
            Thread* holder = nullptr;
            Thread* const waiter = seat_waiter_over_holder(&holder);
            g_sem = semaphore(nullptr);
            wake_next_park(post_the_semaphore);

            sem_wait(g_sem);

            EXPECT_EQ(g_sem->count, 0) << "the token went straight to the waiter";
            EXPECT_TRUE(g_sem->waiters.head == nullptr) << "and it is off the queue";
            EXPECT_EQ(waiter->wait_result, WAIT_RESULT_POISON)
                << "sem_post writes no result, so the poison is what stays";
            EXPECT_STREQ(trace(), "switch2>1 switch1>2") << "the park switched away and back";
        }

        // A waker that ends the park without writing a result leaves the poison, so the
        // fiction is a value no assertion accepts -- where a zeroed TCB would have read as
        // a lock cleanly acquired.
        TEST_F(ParkResult, a_waker_that_writes_no_result_leaves_the_poison)
        {
            Thread* holder = nullptr;
            seat_waiter_over_holder(&holder);
            int handle = 0;
            g_mutex = own_mutex(holder, &handle);
            wake_next_park(end_the_park_writing_nothing);

            int const rc = mutex_lock(g_mutex);

            EXPECT_EQ(rc, static_cast<int>(WAIT_RESULT_POISON))
                << "the return carries the fixture's poison, not a waker's answer";
            EXPECT_NE(rc, 0) << "and it is not the value a zeroed TCB would have given";
        }

        // --- the mechanism's own controls ---------------------------------------------

        // The tripwire keys on a PARK, not on every switch: a thread the scheduler merely
        // rotated is READY and needs no waker.
        TEST_F(ParkResult, a_switch_that_is_not_a_park_needs_no_waker)
        {
            spawn(0, PRIO_HOLDER);
            spawn(1, PRIO_HOLDER);
            sched::reschedule();
            trace_reset();
            g_switches = 0;

            sched::yield();

            EXPECT_EQ(g_switches, 1u) << "the rotation switched";
            EXPECT_STREQ(trace(), "switch1>2") << "and no waker was demanded of it";
        }

        TEST_F(ParkResultDeathTest, a_park_with_no_waker_armed_ends_the_arm)
        {
            Thread* holder = nullptr;
            seat_waiter_over_holder(&holder);
            int handle = 0;
            g_mutex = own_mutex(holder, &handle);

            KICKOS_EXPECT_FIXTURE_REFUSAL(mutex_lock(g_mutex), "no waker armed for the park");
        }
    }
}
