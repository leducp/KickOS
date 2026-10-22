// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The plain word pair over the real sync.cc, park.cc and time.cc: what a waiter answers and
// where a unit ends up when its deadline, a wake and a cancel reach the park, in both orders, the
// wake coming from the peer core above one kernel core; and what a wake does to the word and to
// the waiters it finds. Built once more over the compare-and-swap of an arch whose store can be
// lost, where an arm changes the word between the wake's load and its swap.

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

#if defined(WORDTIMED_BOUNDED)
#include <kickos/arch/atomic_cas.h>

namespace
{
    // The value the next swap attempt finds in the word in place of the one the wake loaded, as
    // a user fast path on another core leaves it.
    bool g_race_armed = false;
    uint32_t g_race_value = 0;
    uint32_t g_swaps = 0;
}

bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    g_swaps++;
    if (g_race_armed)
    {
        g_race_armed = false;
        *word = g_race_value;
    }
    *observed = *word;
    if (*observed != want)
    {
        return false;
    }
    *word = desired;
    return true;
}
#endif

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;
            constexpr uint8_t PRIO_BELOW = 3;
            constexpr uint8_t PRIO_ABOVE = 7;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr int SLOT_FIRST = 2;
            constexpr int SLOT_LAST = 3;
            constexpr int SLOT_EXTRA = 5;
            constexpr uint32_t TIMEOUT_US = 1000;
            constexpr uint32_t PARKED = KOS_WORD_WAITERS;
#if KICKOS_KERNEL_CORES > 1
            constexpr uint32_t CORE_WAKER = KICKOS_KERNEL_CORES - 1;
            constexpr uint8_t PRIO_WAKER = 9;
            constexpr int SLOT_WAKER = 4;
#endif

            uint32_t g_word = 0;

            uintptr_t word_va()
            {
                return reinterpret_cast<uintptr_t>(&g_word);
            }

            WordRecord* record()
            {
                for (WordRecord* m = kernel().word_active; m != nullptr; m = m->next_active)
                {
                    if (m->key == static_cast<arch_phys_addr_t>(word_va()))
                    {
                        return m;
                    }
                }
                return nullptr;
            }

            // A wake from the core a peer thread runs on.
            int wake_from_peer(uint32_t n, uint32_t units)
            {
#if KICKOS_KERNEL_CORES > 1
                uint32_t const was = g_core;
                g_core = CORE_WAKER;
                int const rc = word_wake(word_va(), n, units);
                g_core = was;
                return rc;
#else
                return word_wake(word_va(), n, units);
#endif
            }

            void post_from_peer()
            {
                EXPECT_GE(wake_from_peer(1u, 1u), 0);
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

            void slay(Thread* parked)
            {
                thread_cancel_kind(parked, CANCEL_SLAY, kickos::IrqLock());
            }

            void slay_then_expire(Thread* parked)
            {
                slay(parked);
                deadline_fires();
            }

            // A semaphore's wait as the user library makes it: the waiters bit set, then the
            // wait on the word holding it.
            int timed_wait(uint32_t timeout_us)
            {
                g_word = g_word | KOS_WORD_WAITERS;
                return word_wait(word_va(), g_word, timeout_us);
            }

            // The waiter is CURRENT, and the holder the only other runnable thread on its core,
            // so the park hands the CPU to the holder and the waker runs as it.
            Thread* seat_waiter_over_holder()
            {
                (void)seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
#if KICKOS_KERNEL_CORES > 1
                Thread* const waker = seat_pool(SLOT_WAKER, PRIO_WAKER);
                seat_running_on(waker, CORE_WAKER);
#endif
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), waiter)
                    << "fixture: the waiter is current";
                g_switches = 0;
                g_word = 0;
                return waiter;
            }

            // A waiter parked by hand behind the one the arm parks, at a priority the wake
            // decides by.
            Thread* g_last = nullptr;

            void queue_last_then_expire(Thread*)
            {
                g_last = seat_pool(SLOT_LAST, PRIO_BELOW);
                park_word_waiter(g_last, record());
                deadline_fires();
            }

            Thread* g_above = nullptr;

            void queue_above_post_then_expire(Thread*)
            {
                g_above = seat_pool(SLOT_FIRST, PRIO_ABOVE);
                park_word_waiter(g_above, record());
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

            // The waiters a wake of several pops behind the one that outranks the waker, and
            // what reaches them while that first wake has the core switched away.
            Thread* g_behind_timed = nullptr;
            Thread* g_behind_untimed = nullptr;
            bool g_switched_away = false;

            void expire_behind_the_first()
            {
                g_switched_away = true;
                deadline_fires();
            }

            void kill_behind_the_first()
            {
                g_switched_away = true;
                cancel(g_behind_untimed);
            }

            void wake_three_switching_at_the_first(SwitchAction during)
            {
                seat_waiter_over_holder();
                Thread* const first = seat_pool(SLOT_FIRST, PRIO_ABOVE);
                g_behind_timed = seat_pool(SLOT_LAST, PRIO_BELOW);
                g_behind_untimed = seat_pool(SLOT_EXTRA, PRIO_BELOW);
                WordRecord* const m = plain_word(&g_word);
                park_word_waiter(first, m);
                park_word_waiter(g_behind_timed, m);
                park_word_waiter(g_behind_untimed, m);
                {
                    IrqLock lock;
                    ktime_deadline_arm(g_behind_timed, TIMEOUT_US, lock);
                }
                g_switched_away = false;
                run_at_next_switch(during);

                EXPECT_EQ(word_wake(word_va(), 3u, 1u), 3);

                EXPECT_TRUE(g_switched_away) << "fixture: the first wake did not switch away";
                EXPECT_EQ(first->wait_result, 1);
                EXPECT_EQ(g_behind_timed->wait_result, 0)
                    << "the deadline reached a waiter the wake had already popped";
                EXPECT_EQ(g_behind_untimed->wait_result, 0)
                    << "the kill reached a waiter the wake had already popped";
                EXPECT_NE(g_behind_timed->state, ThreadState::BLOCKED);
                EXPECT_NE(g_behind_untimed->state, ThreadState::BLOCKED);
                EXPECT_EQ(kernel().sleepq, nullptr) << "a popped waiter's deadline stayed queued";
                EXPECT_EQ(record(), nullptr);
                EXPECT_EQ(g_word, 0u);
            }

            void wait_while_cancelled()
            {
                (void)timed_wait(KOS_TIMEOUT_NONE);
            }

            bool queue_is(Thread const* first, Thread const* second)
            {
                WordRecord const* const m = record();
                if (m == nullptr)
                {
                    return false;
                }
                ListNode const* const h = m->waiters.head;
                return h == &first->link and h->next == &second->link and h->next->next == nullptr
                       and m->waiters.tail == &second->link;
            }

            struct Waker
            {
                char const* name;
                ParkWaker waker;
                uint32_t timeout_us;
                int rc;
                uint32_t word;
                uint8_t kind;
            };

            class WordWaker : public KSeam, public ::testing::WithParamInterface<Waker>
            {
            };

            class WordTimed : public KSeam
            {
            };
        }

        TEST_P(WordWaker, answers_its_waker_and_leaves_no_waiter_or_deadline)
        {
            Waker const& w = GetParam();
            Thread* const waiter = seat_waiter_over_holder();
            wake_next_park(w.waker);

            EXPECT_EQ(timed_wait(w.timeout_us), w.rc);
            EXPECT_EQ(g_word, w.word);
            EXPECT_EQ(waiter->cancel_kind, w.kind);
            EXPECT_EQ(record(), nullptr) << "the emptied record was kept";
            EXPECT_EQ(kernel().sleepq, nullptr) << "the waiter's deadline is still queued";
        }

        // A unit handed over answers 1 and leaves the word empty with its bit cleared; a waiter
        // that left by its deadline or a cancel leaves its bit for the next wake to clear, and a
        // wake finding nobody banks its unit.
        INSTANTIATE_TEST_SUITE_P(
            Park, WordWaker,
            ::testing::Values(
                Waker{"a_wake_landing_first_at_the_deadline_hands_its_unit", post_then_expire,
                      TIMEOUT_US, 1, 0u, CANCEL_NONE},
                Waker{"an_expiry_landing_first_leaves_the_wake_banked", expire_then_post,
                      TIMEOUT_US, -KOS_ETIMEDOUT, 1u, CANCEL_NONE},
                Waker{"a_cancel_answers_ecanceled_before_the_deadline", cancel_then_expire,
                      TIMEOUT_US, -KOS_ECANCELED, PARKED, CANCEL_KILL},
                Waker{"a_slay_answers_ecanceled_before_the_deadline", slay_then_expire,
                      TIMEOUT_US, -KOS_ECANCELED, PARKED, CANCEL_SLAY},
                Waker{"an_untimed_cancel_answers_ecanceled", cancel, KOS_TIMEOUT_NONE,
                      -KOS_ECANCELED, PARKED, CANCEL_KILL}),
            [](::testing::TestParamInfo<Waker> const& p)
            {
                return std::string{p.param.name};
            });

        TEST_F(WordTimed, a_try_or_a_changed_word_answers_at_once_with_nothing_parked)
        {
            seat_waiter_over_holder();

            // No waker is armed: a park here ends the arm with the fixture's refusal.
            EXPECT_EQ(timed_wait(0), -KOS_ETIMEDOUT) << "a try waited";
            g_word = 3u;
            EXPECT_EQ(word_wait(word_va(), 2u, TIMEOUT_US), -KOS_EAGAIN)
                << "a word that is not the expected value parked its caller";
            EXPECT_EQ(g_word, 3u) << "a refused wait wrote the word";
            EXPECT_EQ(g_switches, 0u) << "a refused wait switched away";
            EXPECT_EQ(record(), nullptr) << "a refused wait took a record";
            EXPECT_EQ(kernel().sleepq, nullptr) << "a refused wait armed a deadline";
        }

        TEST_F(WordTimed, a_wake_with_no_waiter_adds_its_units_and_clears_the_bit)
        {
            seat_waiter_over_holder();
            g_word = PARKED | 4u;
            EXPECT_EQ(wake_from_peer(2u, 2u), 0) << "a wake with nobody parked woke someone";
            EXPECT_EQ(g_word, 6u);
            EXPECT_EQ(wake_from_peer(0u, 0u), 0);
            EXPECT_EQ(g_word, 6u) << "an empty wake changed the count";
            g_word = PARKED | 6u;
            EXPECT_EQ(wake_from_peer(0u, 0u), 0);
            EXPECT_EQ(g_word, 6u) << "an empty wake left a stale bit set";
        }

        TEST_F(WordTimed, a_wake_hands_its_unit_to_the_highest_waiter_fifo_among_equals_and_adds_nothing)
        {
            seat_waiter_over_holder();
            Thread* const first = seat_pool(SLOT_FIRST, PRIO_BELOW);
            Thread* const high = seat_pool(SLOT_EXTRA, PRIO_ABOVE);
            Thread* const last = seat_pool(SLOT_LAST, PRIO_BELOW);
            WordRecord* const m = plain_word(&g_word);
            park_word_waiter(first, m);
            park_word_waiter(high, m);
            park_word_waiter(last, m);

            EXPECT_EQ(wake_from_peer(1u, 1u), 1);
            EXPECT_EQ(high->wait_result, 1) << "the unit did not go to the highest waiter";
            EXPECT_EQ(first->wait_result, WAIT_RESULT_POISON);
            EXPECT_EQ(g_word, PARKED) << "a unit was added beside a waiter still parked";

            EXPECT_EQ(wake_from_peer(1u, 1u), 1);
            EXPECT_EQ(first->wait_result, 1) << "the unit did not go to the first of two equals";
            EXPECT_EQ(last->wait_result, WAIT_RESULT_POISON);
            EXPECT_EQ(g_word, PARKED);

            EXPECT_EQ(wake_from_peer(1u, 1u), 1);
            EXPECT_EQ(last->wait_result, 1);
            EXPECT_EQ(g_word, 0u) << "the last waiter's wake left the bit set or banked its unit";
            EXPECT_EQ(record(), nullptr) << "the emptied record was kept";
        }

        TEST_F(WordTimed, a_wake_of_more_waiters_than_units_wakes_the_rest_without_one)
        {
            seat_waiter_over_holder();
            Thread* const high = seat_pool(SLOT_FIRST, PRIO_ABOVE);
            Thread* const low = seat_pool(SLOT_LAST, PRIO_BELOW);
            WordRecord* const m = plain_word(&g_word);
            park_word_waiter(low, m);
            park_word_waiter(high, m);

            EXPECT_EQ(wake_from_peer(3u, 1u), 2) << "the wake did not answer how many it woke";
            EXPECT_EQ(high->wait_result, 1);
            EXPECT_EQ(low->wait_result, 0) << "a waiter beyond the units was handed one";
            EXPECT_NE(low->state, ThreadState::BLOCKED);
            EXPECT_EQ(g_word, 0u);
            EXPECT_EQ(record(), nullptr);
        }

        // A sharer can write the word: one that clears the bit under a parked waiter.
        TEST_F(WordTimed, a_wake_reaches_a_waiter_whose_bit_a_sharer_cleared)
        {
            seat_waiter_over_holder();
            Thread* const parked = seat_pool(SLOT_FIRST, PRIO_ABOVE);
            WordRecord* const m = plain_word(&g_word);
            park_word_waiter(parked, m);
            g_word = 0u;

            EXPECT_EQ(wake_from_peer(1u, 1u), 1) << "the wake went by the bit, not the record";
            EXPECT_EQ(parked->wait_result, 1);
            EXPECT_NE(parked->state, ThreadState::BLOCKED);
            EXPECT_EQ(g_word, 0u);
            EXPECT_EQ(record(), nullptr);
        }

        // Every waiter a wake pops is ready before the core can leave the waker, so neither its
        // deadline nor a kill can find it off the queue and still parked.
        TEST_F(WordTimed, a_deadline_during_the_first_wake_of_several_finds_the_rest_ready)
        {
            wake_three_switching_at_the_first(expire_behind_the_first);
        }

        TEST_F(WordTimed, a_kill_during_the_first_wake_of_several_finds_the_rest_ready)
        {
            wake_three_switching_at_the_first(kill_behind_the_first);
        }

        TEST_F(WordTimed, more_units_than_waiters_and_an_overflow_are_refused_with_nobody_woken)
        {
            seat_waiter_over_holder();
            Thread* const parked = seat_pool(SLOT_FIRST, PRIO_BELOW);
            park_word_waiter(parked, plain_word(&g_word));

            EXPECT_EQ(wake_from_peer(1u, 2u), -KOS_EINVAL) << "units above n were taken";
            EXPECT_EQ(parked->wait_kind, WAIT_WORD) << "a refused wake woke a waiter";

            g_word = PARKED | static_cast<uint32_t>(KOS_SEM_COUNT_MAX);
            EXPECT_EQ(wake_from_peer(2u, 2u), -KOS_EOVERFLOW)
                << "the unit left over carried out of the count";
            EXPECT_EQ(parked->wait_kind, WAIT_WORD) << "a refused wake woke a waiter";
            EXPECT_EQ(parked->wait_result, WAIT_RESULT_POISON);
            EXPECT_EQ(g_word, PARKED | static_cast<uint32_t>(KOS_SEM_COUNT_MAX));

            EXPECT_EQ(wake_from_peer(1u, 1u), 1) << "a unit handed over cannot overflow";
            EXPECT_EQ(parked->wait_result, 1);
            EXPECT_EQ(g_word, static_cast<uint32_t>(KOS_SEM_COUNT_MAX));
        }

        TEST_F(WordTimed, a_mutex_call_on_a_plain_word_and_a_word_call_on_a_mutex_are_refused)
        {
            Thread* const holder = seat_pool(SLOT_FIRST, PRIO_BELOW);
            seat_waiter_over_holder();
            Thread* const parked = seat_pool(SLOT_LAST, PRIO_BELOW);
            park_word_waiter(parked, plain_word(&g_word));
            uint32_t const plain = g_word;
            EXPECT_EQ(mutex_lock(word_va(), TIMEOUT_US), -KOS_EINVAL);
            EXPECT_EQ(mutex_unlock(word_va()), -KOS_EINVAL);
            EXPECT_EQ(g_word, plain);
            EXPECT_EQ(parked->wait_kind, WAIT_WORD);

            static uint32_t pi_word = 0;
            Thread* const pi_waiter = seat_pool(SLOT_EXTRA, PRIO_BELOW);
            park_mutex_waiter(pi_waiter, own_mutex(holder, &pi_word));
            uint32_t const held = pi_word;
            uintptr_t const pi = reinterpret_cast<uintptr_t>(&pi_word);
            EXPECT_EQ(word_wait(pi, held, TIMEOUT_US), -KOS_EINVAL);
            EXPECT_EQ(word_wake(pi, 1u, 1u), -KOS_EINVAL);
            EXPECT_EQ(pi_word, held);
            EXPECT_EQ(pi_waiter->wait_kind, WAIT_MUTEX);
        }

        TEST_F(WordTimed, a_pending_cancel_ends_the_wait_before_it_parks)
        {
            Thread* const waiter = seat_waiter_over_holder();
            {
                IrqLock lock;
                thread_cancel(waiter, lock);
            }

            run_noreturn(wait_while_cancelled);

            EXPECT_EQ(waiter->state, ThreadState::EXITED) << "a cancelled caller parked";
            EXPECT_EQ(record(), nullptr) << "a cancelled caller took a record";
        }

        TEST_F(WordTimed, an_abort_frees_the_record_its_last_waiter_leaves)
        {
            seat_waiter_over_holder();
            Thread* const a = seat_pool(SLOT_FIRST, PRIO_BELOW);
            Thread* const b = seat_pool(SLOT_LAST, PRIO_BELOW);
            WordRecord* const m = plain_word(&g_word);
            park_word_waiter(a, m);
            park_word_waiter(b, m);
            {
                IrqLock lock;
                thread_cancel(a, lock);
            }
            EXPECT_EQ(record(), m) << "a record with a waiter left was freed";
            EXPECT_EQ(a->wait_result, -KOS_ECANCELED);
            {
                IrqLock lock;
                thread_cancel(b, lock);
            }
            EXPECT_EQ(record(), nullptr) << "the emptied record was kept";
            EXPECT_EQ(g_word, PARKED) << "an abort wrote the word";
        }

        // The slot a slain timed waiter leaves is the next thread's: a deadline left queued on
        // it would fire into a thread that never waited.
        TEST_F(WordTimed, a_slot_reused_after_a_slain_timed_waiter_carries_no_deadline)
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

        TEST_F(WordTimed, an_expired_waiter_in_the_middle_is_unlinked_alone)
        {
            seat_waiter_over_holder();
            Thread* const first = seat_pool(SLOT_FIRST, PRIO_BELOW);
            park_word_waiter(first, plain_word(&g_word));
            wake_next_park(queue_last_then_expire);

            int const rc = timed_wait(TIMEOUT_US);

            EXPECT_EQ(rc, -KOS_ETIMEDOUT);
            EXPECT_TRUE(queue_is(first, g_last))
                << "the expiry unlinked some other waiter than the one whose deadline fired";
            EXPECT_EQ(g_word, PARKED);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

        TEST_F(WordTimed, a_wake_goes_to_the_highest_waiter_with_a_timed_one_queued_first)
        {
            seat_waiter_over_holder();
            wake_next_park(queue_above_post_then_expire);

            int const rc = timed_wait(TIMEOUT_US);

            EXPECT_EQ(g_above->wait_result, 1) << "the wake did not go to the highest waiter";
            EXPECT_NE(g_above->state, ThreadState::BLOCKED);
            EXPECT_EQ(rc, -KOS_ETIMEDOUT) << "the timed waiter queued first took the unit";
            EXPECT_EQ(record(), nullptr);
            EXPECT_EQ(g_word, PARKED) << "the wake banked a unit beside a parked waiter";
        }

        TEST_F(WordTimed, an_untimed_wait_never_reaches_the_timer)
        {
            seat_waiter_over_holder();
            wake_next_park(note_the_timer_then_post);

            int const rc = timed_wait(KOS_TIMEOUT_NONE);

            EXPECT_EQ(rc, 1);
            EXPECT_EQ(g_word, 0u) << "the unit went to the count, not to the waiter";
            EXPECT_EQ(g_sleepq_while_parked, nullptr) << "an untimed park queued a deadline";
            EXPECT_FALSE(g_on_timer_while_parked);
            EXPECT_EQ(kernel().sleepq, nullptr);
        }

#if defined(WORDTIMED_BOUNDED)
        TEST_F(WordTimed, a_word_changed_between_the_load_and_the_swap_is_woken_from_afresh)
        {
            seat_waiter_over_holder();
            g_word = 5u;
            g_swaps = 0;
            // A taker's decrement lands between the wake's load of 5 and its swap.
            g_race_value = 4u;
            g_race_armed = true;

            EXPECT_EQ(wake_from_peer(1u, 1u), 0);

            EXPECT_FALSE(g_race_armed) << "the wake made no compare-and-swap";
            EXPECT_GE(g_swaps, 2u) << "the wake did not retry its lost swap";
            EXPECT_EQ(g_word, 5u) << "the wake committed over a value it did not load: a unit "
                                     "was created or lost";
        }

        TEST_F(WordTimed, a_waiter_is_popped_only_once_the_swap_lands)
        {
            seat_waiter_over_holder();
            Thread* const parked = seat_pool(SLOT_FIRST, PRIO_BELOW);
            park_word_waiter(parked, plain_word(&g_word));
            g_swaps = 0;
            // A stale-bit clear by a writer lands between the load and the swap.
            g_race_value = 0u;
            g_race_armed = true;

            EXPECT_EQ(wake_from_peer(1u, 1u), 1);

            EXPECT_GE(g_swaps, 2u) << "the wake did not retry its lost swap";
            EXPECT_EQ(parked->wait_result, 1) << "the waiter was not handed the unit once";
            EXPECT_EQ(g_word, 0u);
            EXPECT_EQ(record(), nullptr);
        }
#endif
    }
}
