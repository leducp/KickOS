// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mutex lock's deadline over the real sync.cc, park.cc and time.cc, on host words: a try, a
// wait that expires, and the waiting forms' grant, owner-died hand-over, refusals and priority
// inheritance, each wake SEQUENCED from the park's own switch.

#include <new>

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
            constexpr int SLOT_CHAIN = SLOT_DONOR + 1;
            constexpr int CHAIN = KICKOS_THREAD_SLOTS - SLOT_CHAIN;
            static_assert(CHAIN >= 3, "a chain needs a few thread slots past the fixture's own");
            constexpr uint32_t TIMEOUT_US = 1000;

            Thread* g_holder = nullptr;
            Thread* g_tail = nullptr;
            Thread* g_far = nullptr;
            uint32_t g_word = 0;
            uint32_t g_tail_word = 0;
            uint32_t g_far_word = 0;
            Mutex* g_tail_mutex = nullptr;
            uint8_t g_holder_prio_parked = 0;
            uint8_t g_tail_prio_parked = 0;
            Thread* g_sleepq_while_parked = nullptr;

            uintptr_t va(uint32_t* word)
            {
                return reinterpret_cast<uintptr_t>(word);
            }

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
                (void)mutex_unlock(va(&g_word));
                deadline_fires();
            }

            void unlock(Thread*)
            {
                note_the_parked_state();
                (void)mutex_unlock(va(&g_word));
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
                (void)mutex_unlock(va(&g_word));
                kernel().current(kickos_kernel_core()) = woken;
            }

            void owner_dies_then_expire(Thread*)
            {
                note_the_parked_state();
                {
                    IrqLock lock;
                    g_holder->dying = true;
                    mutex_owner_exit(g_holder, lock);
                }
                deadline_fires();
            }

            // The waiter is CURRENT and the holder the only other runnable thread, so the park
            // hands the CPU to the holder and the waker runs as it. The holder took the word
            // uncontended: no record.
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
                g_tail_mutex = nullptr;
                g_holder_prio_parked = 0;
                g_tail_prio_parked = 0;
                g_sleepq_while_parked = nullptr;
                g_word = g_holder->lock_id;
                g_tail_word = 0;
                g_far_word = 0;
                return waiter;
            }

            // The holder parked on a second word the tail holds, the tail inheriting it: the
            // waiter's boost then runs two links.
            void holder_waits_on_the_tail()
            {
                g_tail = seat_pool(SLOT_TAIL, PRIO_TAIL);
                g_tail_mutex = own_mutex(g_tail, &g_tail_word);
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

            // The tail parked in turn on a third word the far owner holds.
            void tail_waits_on_the_far_owner()
            {
                g_far = seat_pool(SLOT_FAR, PRIO_FAR);
                Mutex* const far_mutex = own_mutex(g_far, &g_far_word);
                park_mutex_waiter(g_tail, far_mutex);
                IrqLock lock;
                sched::set_prio(g_far, thread_effective_prio(g_far), lock);
            }
        }

        TEST_F(MutexTimed, a_try_takes_a_free_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            uint32_t free_word = 0;

            EXPECT_EQ(mutex_lock(va(&free_word), 0), 0) << "a try refused a free mutex";
            EXPECT_EQ(free_word, waiter->lock_id);
            EXPECT_EQ(waiter->held_list, nullptr) << "an uncontended take made a record";
            EXPECT_EQ(mutex_lock(va(&free_word), 0), -KOS_EDEADLK) << "a try on its own mutex";
            EXPECT_EQ(mutex_unlock(va(&free_word)), 0);
            EXPECT_EQ(free_word, 0u);
            EXPECT_EQ(g_switches, 0u);
        }

        // No waker is armed: a park here ends the arm with the fixture's refusal. The second
        // leg finds a record, which a try must answer as early as a bare held word.
        TEST_F(MutexTimed, a_try_on_a_held_mutex_answers_at_once_and_lends_nothing)
        {
            seat_waiter_over_holder();

            EXPECT_EQ(mutex_lock(va(&g_word), 0), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_word, g_holder->lock_id);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER) << "a failed try boosted the holder";
            EXPECT_EQ(kernel().mutex_active, nullptr) << "a failed try queued itself";
            EXPECT_EQ(g_switches, 0u) << "a failed try switched away";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a failed try armed a deadline";

            Mutex* const m = own_mutex(g_holder, &g_word);
            donor_waits_on(m, PRIO_PARTIAL);
            Thread* const donor = thread_of(m->waiters.head);

            EXPECT_EQ(mutex_lock(va(&g_word), 0), -KOS_ETIMEDOUT) << "a try with a waiter parked";
            EXPECT_EQ(g_holder->prio, PRIO_PARTIAL)
                << "a failed try lent the holder more than the parked waiter gave it";
            EXPECT_EQ(m->waiters.head, m->waiters.tail) << "a failed try queued itself";
            EXPECT_EQ(thread_of(m->waiters.head), donor);
            EXPECT_EQ(g_switches, 0u) << "a failed try switched away";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a failed try armed a deadline";
        }

        // While a thread waits the record names the holder, whatever the word says: a sharer
        // that does not hold it cannot hand it on, even through a word rewritten to name itself.
        TEST_F(MutexTimed, an_unlock_of_a_contended_word_by_a_non_holder_is_refused)
        {
            Thread* const waiter = seat_waiter_over_holder();
            Mutex* const m = own_mutex(g_holder, &g_word);
            donor_waits_on(m, PRIO_PARTIAL);
            Thread* const donor = thread_of(m->waiters.head);
            uint32_t const held = g_word;

            EXPECT_EQ(mutex_unlock(va(&g_word)), -KOS_EPERM);
            EXPECT_EQ(g_word, held) << "a refused unlock wrote the word";
            g_word = waiter->lock_id | KOS_MUTEX_WAITERS;
            EXPECT_EQ(mutex_unlock(va(&g_word)), -KOS_EPERM) << "the word, not the record, decided";
            EXPECT_EQ(m->owner, g_holder);
            EXPECT_EQ(g_holder->held_list, m);
            EXPECT_EQ(thread_of(m->waiters.head), donor) << "a refused unlock woke the waiter";
            EXPECT_EQ(donor->wait_kind, WAIT_MUTEX);
        }

        // No waker is armed: a park here ends the arm with the fixture's refusal.
        TEST_F(MutexTimed, a_word_disagreeing_with_its_record_is_refused_and_left_alone)
        {
            Thread* const waiter = seat_waiter_over_holder();
            Mutex* const m = own_mutex(g_holder, &g_word);
            donor_waits_on(m, PRIO_PARTIAL);

            g_word = 0;
            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_EINVAL) << "a cleared word";
            EXPECT_EQ(g_word, 0u) << "a refused lock wrote the word";
            g_word = waiter->lock_id;
            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_EINVAL)
                << "a word naming the caller";
            EXPECT_EQ(g_word, waiter->lock_id);
            EXPECT_EQ(m->owner, g_holder);
            EXPECT_EQ(m->waiters.head, m->waiters.tail) << "a refused lock queued itself";
            EXPECT_EQ(g_holder->prio, PRIO_PARTIAL) << "a refused lock boosted the holder";
            EXPECT_EQ(g_switches, 0u);
            EXPECT_EQ(kernel().sleepq, nullptr) << "a refused lock armed a deadline";
        }

        // Set again, the bit keeps the holder's release in the kernel, where the waiter is.
        TEST_F(MutexTimed, a_cleared_waiters_bit_is_set_again)
        {
            seat_waiter_over_holder();
            Mutex* const m = own_mutex(g_holder, &g_word);
            donor_waits_on(m, PRIO_PARTIAL);

            g_word = g_holder->lock_id;
            EXPECT_EQ(mutex_lock(va(&g_word), 0), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_word, g_holder->lock_id | KOS_MUTEX_WAITERS);
            EXPECT_EQ(m->waiters.head, m->waiters.tail) << "a failed try queued itself";
        }

        // A try answers it too, which the cycle walk alone would not reach.
        TEST_F(MutexTimed, a_take_of_a_contended_word_the_caller_holds_is_a_deadlock)
        {
            Thread* const waiter = seat_waiter_over_holder();
            uint32_t mine_word = 0;
            Mutex* const mine = own_mutex(waiter, &mine_word);
            park_mutex_waiter(g_holder, mine);
            uint32_t const held = mine_word;

            EXPECT_EQ(mutex_lock(va(&mine_word), 0), -KOS_EDEADLK);
            EXPECT_EQ(mutex_lock(va(&mine_word), TIMEOUT_US), -KOS_EDEADLK);
            EXPECT_EQ(mine_word, held);
            EXPECT_EQ(mine->waiters.head, mine->waiters.tail) << "the holder queued on itself";
            EXPECT_EQ(g_switches, 0u);
        }

        TEST_F(MutexTimed, a_timed_wait_expires_and_its_boost_is_reverted)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_holder_prio_parked, PRIO_WAITER) << "the waiting form lent nothing";
            EXPECT_NE(g_sleepq_while_parked, nullptr) << "the wait armed no deadline";
            EXPECT_EQ(g_word & ~KOS_MUTEX_WAITERS, g_holder->lock_id);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER) << "the expired waiter's boost outlived it";
            EXPECT_EQ(kernel().mutex_active, nullptr) << "the emptied record was kept";
            EXPECT_EQ(g_holder->held_list, nullptr);
            EXPECT_EQ(waiter->wait_kind, WAIT_NONE);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(MutexTimed, an_expiry_reverts_the_boost_along_the_whole_chain)
        {
            seat_waiter_over_holder();
            holder_waits_on_the_tail();
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
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

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ECANCELED);
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
            donor_waits_on(own_mutex(g_holder, &g_word), PRIO_PARTIAL);
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
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

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_tail->prio, PRIO_WAITER) << "the other donor's boost on the tail was lost";
            EXPECT_EQ(g_far->prio, PRIO_WAITER) << "the owner past the tail was lowered";
        }

        TEST_F(MutexTimed, an_unlock_after_the_expiry_frees_the_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(expire_then_unlock);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_word, 0u) << "the unlock handed the mutex to the expired waiter";
            EXPECT_EQ(waiter->held_list, nullptr);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_holder->held_list, nullptr);
        }

        TEST_F(MutexTimed, an_unlock_before_the_deadline_grants_the_mutex)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(unlock_then_expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), 0);
            EXPECT_EQ(g_word, waiter->lock_id) << "the hand-over wrote another owner";
            EXPECT_EQ(kernel().mutex_active, nullptr) << "the emptied record was kept";
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(kernel().sleepq, nullptr) << "the granted waiter's deadline is still queued";
        }

        TEST_F(MutexTimed, an_owner_dying_before_the_deadline_hands_the_mutex_over_held)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(owner_dies_then_expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_EOWNERDEAD);
            EXPECT_EQ(g_word, waiter->lock_id) << "the death wrote another owner";
            EXPECT_EQ(g_holder->held_list, nullptr);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(MutexTimed, an_untimed_wait_never_reaches_the_timer)
        {
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(unlock);

            EXPECT_EQ(mutex_lock(va(&g_word), KOS_TIMEOUT_NONE), 0);
            EXPECT_EQ(g_holder_prio_parked, PRIO_WAITER);
            EXPECT_EQ(g_sleepq_while_parked, nullptr) << "an untimed wait queued a deadline";
            EXPECT_EQ(g_word, waiter->lock_id);
        }

        // A holder that dies with nobody waiting leaves its id in the word for the next taker,
        // which steals it once, by a waiting lock and by a try alike.
        TEST_F(MutexTimed, a_death_with_no_waiter_reaches_the_next_taker_once)
        {
            Thread* const waiter = seat_waiter_over_holder();
            uint32_t second = g_holder->lock_id;
            g_holder->dying = true;

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_EOWNERDEAD);
            EXPECT_EQ(g_word, waiter->lock_id) << "the owner-died answer is an acquire";
            EXPECT_EQ(mutex_unlock(va(&g_word)), 0);
            EXPECT_EQ(mutex_lock(va(&g_word), 0), 0) << "the death was reported twice";
            EXPECT_EQ(mutex_unlock(va(&g_word)), 0);

            EXPECT_EQ(mutex_lock(va(&second), 0), -KOS_EOWNERDEAD) << "a try missed the death";
            EXPECT_EQ(mutex_unlock(va(&second)), 0);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_switches, 0u);
        }

        // A slot still being built carries its id but has held nothing: a word naming it is a
        // dead holder's, stolen with nothing lent and no park (no waker is armed).
        TEST_F(MutexTimed, a_word_naming_a_slot_being_built_is_a_dead_holders)
        {
            Thread* const waiter = seat_waiter_over_holder();
            Thread* const built = new (&kernel().threads.slots[SLOT_FAR]) Thread{};
            built->lock_id = kernel().threads.lock_id_for(SLOT_FAR);
            ASSERT_TRUE(built->state == ThreadState::INACTIVE);
            uint32_t word = built->lock_id;

            EXPECT_EQ(mutex_lock(va(&word), TIMEOUT_US), -KOS_EOWNERDEAD);
            EXPECT_EQ(word, waiter->lock_id);
            EXPECT_EQ(built->prio, 0u) << "a thread being built was lent a priority";
            EXPECT_EQ(kernel().mutex_active, nullptr);
            EXPECT_EQ(g_switches, 0u);
            EXPECT_EQ(mutex_unlock(va(&word)), 0);
        }

        // The active list in order from its head, as far as `room` records.
        int active_records(Mutex** out, int room)
        {
            int n = 0;
            for (Mutex* m = kernel().mutex_active; m != nullptr and n < room; m = m->next_active)
            {
                out[n] = m;
                n = n + 1;
            }
            return n;
        }

        // A record freed from the middle of the active list, then its oldest, then its newest:
        // each end of a wait leaves exactly the other records listed.
        TEST_F(MutexTimed, an_ended_wait_frees_its_record_from_anywhere_in_the_active_list)
        {
            Thread* const holder = seat_pool(SLOT_TAIL, PRIO_FAR);
            Thread* waiters[3];
            uint32_t words[3] = {0, 0, 0};
            Mutex* records[3];
            for (int i = 0; i < 3; i++)
            {
                waiters[i] = seat_pool(SLOT_FAR + i, PRIO_FAR);
                records[i] = own_mutex(holder, &words[i]);
                park_mutex_waiter(waiters[i], records[i]);
            }
            Mutex* seen[4];
            ASSERT_EQ(active_records(seen, 4), 3);
            ASSERT_EQ(seen[0], records[2]);
            ASSERT_EQ(seen[1], records[1]);
            ASSERT_EQ(seen[2], records[0]);

            {
                IrqLock lock;
                thread_abort_park(waiters[1], -KOS_ECANCELED, lock);
            }
            ASSERT_EQ(active_records(seen, 4), 2) << "the middle record was kept, or another lost";
            EXPECT_EQ(seen[0], records[2]);
            EXPECT_EQ(seen[1], records[0]);

            {
                IrqLock lock;
                thread_abort_park(waiters[0], -KOS_ECANCELED, lock);
            }
            ASSERT_EQ(active_records(seen, 4), 1) << "the oldest record was kept, or another lost";
            EXPECT_EQ(seen[0], records[2]);

            {
                IrqLock lock;
                thread_abort_park(waiters[2], -KOS_ECANCELED, lock);
            }
            EXPECT_EQ(kernel().mutex_active, nullptr) << "the last record was kept";
            EXPECT_EQ(holder->held_list, nullptr);
            for (int i = 0; i < 3; i++)
            {
                EXPECT_EQ(records[i]->owner, nullptr) << "record " << i << " was not freed";
            }
        }

        Thread* g_older_waiter = nullptr;
        Mutex* g_older_seen[4];
        int g_older_seen_n = 0;
        arch_phys_addr_t g_older_seen_key = 0;

        void free_the_older_then_expire(Thread*)
        {
            {
                IrqLock lock;
                thread_abort_park(g_older_waiter, -KOS_ECANCELED, lock);
            }
            g_older_seen_n = active_records(g_older_seen, 4);
            if (g_older_seen_n > 0)
            {
                g_older_seen_key = g_older_seen[0]->key;
            }
            deadline_fires();
        }

        // The lock's record goes in at the head, ahead of a record already listed: that older
        // record's end, while the lock still waits, leaves the new one listed.
        TEST_F(MutexTimed, a_record_the_lock_takes_stays_listed_when_an_older_one_is_freed)
        {
            seat_waiter_over_holder();
            Thread* const other_holder = seat_pool(SLOT_TAIL, PRIO_FAR);
            g_older_waiter = seat_pool(SLOT_FAR, PRIO_FAR);
            uint32_t older_word = 0;
            Mutex* const older = own_mutex(other_holder, &older_word);
            park_mutex_waiter(g_older_waiter, older);
            g_older_seen_n = 0;
            g_older_seen_key = 0;
            wake_next_park(free_the_older_then_expire);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_ETIMEDOUT);
            ASSERT_EQ(g_older_seen_n, 1) << "the lock's record left the list with the older one";
            EXPECT_NE(g_older_seen[0], older);
            EXPECT_EQ(g_older_seen_key, static_cast<arch_phys_addr_t>(va(&g_word)));
            EXPECT_EQ(kernel().mutex_active, nullptr) << "the emptied record was kept";
        }

        // The holder waits on a word the waiter holds, so the waiter's lock closes a cycle.
        TEST_F(MutexTimed, a_timed_wait_that_closes_a_cycle_is_refused_before_it_parks)
        {
            Thread* const waiter = seat_waiter_over_holder();
            uint32_t mine_word = 0;
            Mutex* const mine = own_mutex(waiter, &mine_word);
            park_mutex_waiter(g_holder, mine);

            EXPECT_EQ(mutex_lock(va(&g_word), TIMEOUT_US), -KOS_EDEADLK);
            EXPECT_EQ(g_holder->prio, PRIO_HOLDER);
            EXPECT_EQ(g_switches, 0u);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        // The boost runs to the end of a chain through every thread slot the fixture leaves, and
        // the expiry takes it back off every link. No thread is on the chain twice, so its length
        // is bounded by the threads.
        Thread* g_chain_tail = nullptr;
        uint8_t g_chain_tail_prio_parked = 0;

        void note_the_chain_tail_then_expire(Thread*)
        {
            g_chain_tail_prio_parked = g_chain_tail->prio;
            deadline_fires();
        }

        // Link i holds words[i] and waits on words[i + 1]; the first word has no waiter yet.
        void seat_chain(Thread** links, uint32_t* words)
        {
            for (int i = 0; i < CHAIN; i++)
            {
                links[i] = seat_pool(SLOT_CHAIN + i, PRIO_FAR);
            }
            words[0] = links[0]->lock_id;
            for (int i = 1; i < CHAIN; i++)
            {
                park_mutex_waiter(links[i - 1], own_mutex(links[i], &words[i]));
            }
        }

        TEST_F(MutexTimed, a_boost_reaches_the_end_of_a_chain_through_every_thread_slot)
        {
            (void)seat_waiter_over_holder();
            Thread* links[CHAIN];
            uint32_t words[CHAIN];
            seat_chain(links, words);
            g_chain_tail = links[CHAIN - 1];
            g_chain_tail_prio_parked = 0;
            wake_next_park(note_the_chain_tail_then_expire);

            EXPECT_EQ(mutex_lock(va(&words[0]), TIMEOUT_US), -KOS_ETIMEDOUT);
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

        // The walk is bounded by the threads, which an acyclic chain visits once each, so a cycle
        // through every thread slot is still found.
        TEST_F(MutexTimed, a_cycle_through_every_thread_slot_is_refused)
        {
            Thread* const waiter = seat_waiter_over_holder();
            uint32_t mine_word = 0;
            Mutex* const mine = own_mutex(waiter, &mine_word);
            Thread* links[CHAIN];
            uint32_t words[CHAIN];
            seat_chain(links, words);
            park_mutex_waiter(links[CHAIN - 1], mine);
            wake_next_park(expire);

            EXPECT_EQ(mutex_lock(va(&words[0]), TIMEOUT_US), -KOS_EDEADLK);
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
