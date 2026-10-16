// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex lock's deadline over the real sync.cc, park.cc and time.cc: a try, a wait that
// expires, and the waiting forms' grant, owner-died hand-over, refusals and priority
// inheritance, each wake SEQUENCED from the park's own switch.

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
            class MutexTimed : public KSeam
            {
            };

            constexpr uint8_t PRIO_FAR = 2;
            constexpr uint8_t PRIO_TAIL = 3;
            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_PARTIAL = 7;
            constexpr uint8_t PRIO_WAITER = 8;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr int SLOT_TAIL = 2;
            constexpr int SLOT_FAR = 3;
            constexpr int SLOT_DONOR = 4;
            constexpr uint32_t TIMEOUT_US = 1000;

            Thread* g_holder = nullptr;
            Thread* g_tail = nullptr;
            Thread* g_far = nullptr;
            Mutex* g_mutex = nullptr;
            Mutex* g_tail_mutex = nullptr;
            uint8_t g_holder_prio_parked = 0;
            uint8_t g_tail_prio_parked = 0;
            Thread* g_sleepq_while_parked = nullptr;

            void deadline_fires()
            {
                g_now_ns += static_cast<uint64_t>(TIMEOUT_US) * 1000u + KICKOS_TIMER_MIN_DELTA_NS;
                ktime_on_timer();
            }

            void note_the_parked_state()
            {
                g_holder_prio_parked = g_holder->prio;
                if (g_tail != nullptr)
                {
                    g_tail_prio_parked = g_tail->prio;
                }
                g_sleepq_while_parked = kernel().sleepq;
            }

            void expire(Thread*)
            {
                note_the_parked_state();
                deadline_fires();
            }

            // The holder runs as current here, which is what lets its unlock pass the owner test.
            void unlock_then_expire(Thread*)
            {
                note_the_parked_state();
                (void)mutex_unlock(g_mutex);
                deadline_fires();
            }

            void unlock(Thread*)
            {
                note_the_parked_state();
                (void)mutex_unlock(g_mutex);
            }

            void cancel(Thread* parked)
            {
                note_the_parked_state();
                thread_cancel(parked, kickos::IrqLock());
            }

            // The expiry readies the waiter above the holder, so the holder's unlock comes later.
            void expire_then_unlock(Thread*)
            {
                note_the_parked_state();
                deadline_fires();
                Thread* const woken = kernel().current(kickos_kernel_core());
                kernel().current(kickos_kernel_core()) = g_holder;
                (void)mutex_unlock(g_mutex);
                kernel().current(kickos_kernel_core()) = woken;
            }

            void owner_dies_then_expire(Thread*)
            {
                note_the_parked_state();
                {
                    IrqLock lock;
                    mutex_force_unlock(g_mutex, g_holder, lock);
                }
                deadline_fires();
            }

            // The waiter is CURRENT and the holder the only other runnable thread, so the park
            // hands the CPU to the holder and the waker runs as it.
            Thread* seat_waiter_over_holder()
            {
                g_holder = seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter)
                    << "fixture: the waiter is current";
                g_switches = 0;
                g_tail = nullptr;
                g_far = nullptr;
                g_holder_prio_parked = 0;
                g_tail_prio_parked = 0;
                g_sleepq_while_parked = nullptr;
                int handle = 0;
                g_mutex = own_mutex(g_holder, &handle);
                return waiter;
            }

            // The holder parked on a second mutex the tail holds, the tail inheriting it: the
            // waiter's boost then runs two links.
            void holder_waits_on_the_tail()
            {
                g_tail = seat_pool(SLOT_TAIL, PRIO_TAIL);
                int handle = 0;
                g_tail_mutex = own_mutex(g_tail, &handle);
                park_mutex_waiter(g_holder, g_tail_mutex);
                IrqLock lock;
                sched::set_prio(g_tail, thread_effective_prio(g_tail), lock);
            }

            // A second donor `prio` parked on `m`, its inheritance seated along the chain.
            void donor_waits_on(Mutex* m, uint8_t prio)
            {
                Thread* const donor = seat_pool(SLOT_DONOR, prio);
                park_mutex_waiter(donor, m);
                IrqLock lock;
                Thread* t = m->owner;
                while (t != nullptr)
                {
                    sched::set_prio(t, thread_effective_prio(t), lock);
                    Mutex const* const next = t->wait_mutex();
                    if (next == nullptr)
                    {
                        break;
                    }
                    t = next->owner;
                }
            }

            // The tail parked in turn on a third mutex the far owner holds.
            void tail_waits_on_the_far_owner()
            {
                g_far = seat_pool(SLOT_FAR, PRIO_FAR);
                int handle = 0;
                Mutex* const far_mutex = own_mutex(g_far, &handle);
                park_mutex_waiter(g_tail, far_mutex);
                IrqLock lock;
                sched::set_prio(g_far, thread_effective_prio(g_far), lock);
            }
        }

        TEST_F(MutexTimed, a_try_takes_a_free_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            Mutex free_mutex;
            mutex_init(&free_mutex);

            EXPECT_EQ(mutex_lock(&free_mutex, 0), 0) << "a try refused a free mutex";
            EXPECT_EQ(free_mutex.owner, waiter);
            EXPECT_EQ(waiter->held_list, &free_mutex);
            EXPECT_EQ(mutex_lock(&free_mutex, 0), -KOS_EDEADLK) << "a try on its own mutex";
            EXPECT_EQ(mutex_unlock(&free_mutex), 0);
            EXPECT_EQ(g_switches, 0u);
        }

        // No waker is armed: a park here ends the arm with the fixture's refusal.
        TEST_F(MutexTimed, a_try_on_a_held_mutex_answers_at_once_and_lends_nothing)
        {
            seat_waiter_over_holder();

            EXPECT_EQ(mutex_lock(g_mutex, 0), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_mutex->owner, g_holder);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER) << "a failed try boosted the holder";
            EXPECT_EQ(g_mutex->waiters.head, nullptr) << "a failed try queued itself";
            EXPECT_EQ(g_switches, 0u) << "a failed try switched away";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a failed try armed a deadline";
        }

        TEST_F(MutexTimed, a_timed_wait_expires_and_its_boost_is_reverted)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_holder_prio_parked, PRIO_WAITER) << "the waiting form lent nothing";
            EXPECT_NE(g_sleepq_while_parked, nullptr) << "the wait armed no deadline";
            EXPECT_EQ(g_mutex->owner, g_holder);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER) << "the expired waiter's boost outlived it";
            EXPECT_EQ(g_mutex->waiters.head, nullptr);
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(MutexTimed, an_expiry_reverts_the_boost_along_the_whole_chain)
        {
            seat_waiter_over_holder();
            holder_waits_on_the_tail();
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_tail_prio_parked, PRIO_WAITER) << "the boost did not cross both links";
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_tail->prio, PRIO_HOLDER)
                << "the tail kept the expired waiter's boost, not its own waiter's";
        }

        TEST_F(MutexTimed, a_cancel_reverts_the_boost_along_the_whole_chain)
        {
            Thread* const waiter = seat_waiter_over_holder();
            holder_waits_on_the_tail();
            wake_next_park(cancel);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ECANCELED);
            EXPECT_NE(waiter->cancel_kind, CANCEL_NONE);
            EXPECT_EQ(g_tail_prio_parked, PRIO_WAITER);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_tail->prio, PRIO_HOLDER) << "the tail kept the cancelled waiter's boost";
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        // The holder keeps a lesser donor of its own: the revert lowers it to that donor and
        // carries the new value on up the chain.
        TEST_F(MutexTimed, an_expiry_lowers_a_partly_boosted_owner_and_carries_on_past_it)
        {
            seat_waiter_over_holder();
            holder_waits_on_the_tail();
            donor_waits_on(g_mutex, PRIO_PARTIAL);
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_holder->prio, PRIO_PARTIAL);
            EXPECT_EQ(g_tail->prio, PRIO_PARTIAL)
                << "the walk stopped at an owner that is still boosted";
        }

        // A mid-chain owner keeps its whole boost from another donor: the revert stops there
        // and leaves the owners past it at that donor's level.
        TEST_F(MutexTimed, an_expiry_stops_at_an_owner_another_donor_keeps_boosted)
        {
            seat_waiter_over_holder();
            holder_waits_on_the_tail();
            tail_waits_on_the_far_owner();
            donor_waits_on(g_tail_mutex, PRIO_WAITER);
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_tail->prio, PRIO_WAITER) << "the other donor's boost on the tail was lost";
            EXPECT_EQ(g_far->prio, PRIO_WAITER) << "the owner past the tail was lowered";
        }

        TEST_F(MutexTimed, an_unlock_after_the_expiry_frees_the_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(expire_then_unlock);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_mutex->owner, nullptr) << "the unlock handed the mutex to the expired waiter";
            EXPECT_EQ(waiter->held_list, nullptr);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_holder->held_list, nullptr);
        }

        TEST_F(MutexTimed, an_unlock_before_the_deadline_grants_the_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(unlock_then_expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), 0);
            EXPECT_EQ(g_mutex->owner, waiter);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(kernel().sleepq, nullptr) << "the granted waiter's deadline is still queued";
        }

        TEST_F(MutexTimed, an_owner_dying_before_the_deadline_hands_the_mutex_over_held)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(owner_dies_then_expire);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_EOWNERDEAD);
            EXPECT_EQ(g_mutex->owner, waiter);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(MutexTimed, an_untimed_wait_never_reaches_the_timer)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(unlock);

            EXPECT_EQ(mutex_lock(g_mutex, KOS_TIMEOUT_NONE), 0);
            EXPECT_EQ(g_holder_prio_parked, PRIO_WAITER);
            EXPECT_EQ(g_sleepq_while_parked, nullptr) << "an untimed wait queued a deadline";
            EXPECT_EQ(g_mutex->owner, waiter);
        }

        // A holder that dies with nobody waiting leaves the death for the next taker, once, by a
        // waiting lock and by a try alike.
        TEST_F(MutexTimed, a_death_with_no_waiter_reaches_the_next_taker_once)
        {
            Thread* const waiter = seat_waiter_over_holder();
            {
                IrqLock lock;
                mutex_force_unlock(g_mutex, g_holder, lock);
            }
            EXPECT_EQ(g_mutex->owner, nullptr);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_EOWNERDEAD);
            EXPECT_EQ(g_mutex->owner, waiter) << "the owner-died answer is an acquire";
            EXPECT_EQ(mutex_unlock(g_mutex), 0);
            EXPECT_EQ(mutex_lock(g_mutex, 0), 0) << "the death was reported twice";
            EXPECT_EQ(mutex_unlock(g_mutex), 0);

            Mutex second;
            mutex_init(&second);
            second.owner = g_holder;
            g_holder->held_list = &second;
            {
                IrqLock lock;
                mutex_force_unlock(&second, g_holder, lock);
            }
            EXPECT_EQ(mutex_lock(&second, 0), -KOS_EOWNERDEAD) << "a try missed the death";
            EXPECT_EQ(mutex_unlock(&second), 0);
            EXPECT_EQ(g_switches, 0u);
        }

        // The holder waits on a mutex the waiter holds, so the waiter's lock closes a cycle.
        TEST_F(MutexTimed, a_timed_wait_that_closes_a_cycle_is_refused_before_it_parks)
        {
            Thread* const waiter = seat_waiter_over_holder();
            int handle = 0;
            Mutex* const mine = own_mutex(waiter, &handle);
            park_mutex_waiter(g_holder, mine);

            EXPECT_EQ(mutex_lock(g_mutex, TIMEOUT_US), -KOS_EDEADLK);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_switches, 0u);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        // The boost runs to the end of a chain longer than the mutex pool, and the expiry takes
        // it back off every link. No thread is on the chain twice, so its length is bounded by
        // the threads, not by the pool.
        Thread* g_chain_tail = nullptr;
        uint8_t g_chain_tail_prio_parked = 0;

        void note_the_chain_tail_then_expire(Thread*)
        {
            g_chain_tail_prio_parked = g_chain_tail->prio;
            deadline_fires();
        }

        TEST_F(MutexTimed, a_boost_reaches_the_end_of_a_chain_longer_than_the_mutex_pool)
        {
            constexpr int CHAIN = KICKOS_MAX_MUTEXES + 2;
            constexpr int SLOT_CHAIN = SLOT_DONOR + 1;
            static_assert(SLOT_CHAIN + CHAIN <= KICKOS_THREAD_SLOTS,
                          "the chain needs a thread slot per link");
            (void)seat_waiter_over_holder();
            Mutex chain[CHAIN];
            Thread* links[CHAIN];
            for (int i = 0; i < CHAIN; i++)
            {
                links[i] = seat_pool(SLOT_CHAIN + i, PRIO_FAR);
                chain[i].owner = links[i];
                links[i]->held_list = &chain[i];
            }
            for (int i = 0; i < CHAIN - 1; i++)
            {
                park_mutex_waiter(links[i], &chain[i + 1]);
            }
            g_chain_tail = links[CHAIN - 1];
            g_chain_tail_prio_parked = 0;
            wake_next_park(note_the_chain_tail_then_expire);

            EXPECT_EQ(mutex_lock(&chain[0], TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_chain_tail_prio_parked, PRIO_WAITER)
                << "the boost stopped short of the chain's end";
            for (int i = 0; i < CHAIN; i++)
            {
                EXPECT_EQ(links[i]->prio, PRIO_FAR) << "link " << i << " kept the expired boost";
            }
            for (int i = 0; i < CHAIN - 1; i++)
            {
                IrqLock lock;
                links[i]->wait_queue->unlink(&links[i]->link);
            }
        }

        // Mutexes outside the pool count too, so a cycle can be longer than the pool is wide; the
        // walk is bounded by the threads, which an acyclic chain visits once each.
        TEST_F(MutexTimed, a_cycle_longer_than_the_mutex_pool_is_refused)
        {
            constexpr int CHAIN = KICKOS_MAX_MUTEXES + 2;
            constexpr int SLOT_CHAIN = SLOT_DONOR + 1;
            static_assert(SLOT_CHAIN + CHAIN <= KICKOS_THREAD_SLOTS,
                          "the chain needs a thread slot per link");
            Thread* const waiter = seat_waiter_over_holder();
            Mutex mine;
            mine.owner = waiter;
            waiter->held_list = &mine;
            Mutex chain[CHAIN];
            Thread* links[CHAIN];
            for (int i = 0; i < CHAIN; i++)
            {
                links[i] = seat_pool(SLOT_CHAIN + i, PRIO_FAR);
                chain[i].owner = links[i];
                links[i]->held_list = &chain[i];
            }
            for (int i = 0; i < CHAIN - 1; i++)
            {
                park_mutex_waiter(links[i], &chain[i + 1]);
            }
            park_mutex_waiter(links[CHAIN - 1], &mine);
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(&chain[0], TIMEOUT_US), -KOS_EDEADLK);
            EXPECT_EQ(g_switches, 0u) << "the waiter parked on a cycle through itself";
            EXPECT_EQ(links[0]->prio, PRIO_FAR) << "a refused lock lent its priority";
            for (int i = 0; i < CHAIN; i++)
            {
                IrqLock lock;
                links[i]->wait_queue->unlink(&links[i]->link);
            }
        }
    }
}
