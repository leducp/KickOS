// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The condition variable over the real user/src/cond.cc and mutex.cc, the sim's compare-and-swap,
// and the real sync.cc, park.cc and time.cc behind the user library's kernel entries: a signal
// landing at each step between the release and the park, a broadcast, a deadline and a kill,
// each SEQUENCED from a park's own switch or from a hook on the waiter's own steps.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/arch/arch.h>
#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

#include <string>

#include "kseam_test.h"

namespace
{
    kos_cond_t g_cond = KOS_COND_INIT;
    kos_mutex_t g_mutex = KOS_MUTEX_INIT;
    // A word no thread waits on, for a syscall after the condition's wait.
    uint32_t g_spare = 0;

    // One-shot: a peer's signal at the waiter's next step of that kind.
    bool g_signal_at_unlock = false;
    bool g_signal_at_cas = false;
    bool g_signal_at_wait = false;

    void peer_signals()
    {
        EXPECT_EQ(kos_cond_signal(&g_cond), 0);
    }
}

// The kernel's syscall entry for the user library's four: a caller with a cancel pending ends at
// any of them but an unlock and a wake.
extern "C" uintptr_t arch_syscall(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                  uintptr_t)
{
    using namespace kickos;
    Thread* const caller = sched::current();
    if (caller->cancel_kind != CANCEL_NONE and not caller->dying and nr != KOS_SYS_MUTEX_UNLOCK
        and nr != KOS_SYS_WORD_WAKE)
    {
        sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN);
    }
    switch (nr)
    {
        case KOS_SYS_MUTEX_LOCK:
        {
            return static_cast<uintptr_t>(mutex_lock(a0, static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_MUTEX_UNLOCK:
        {
            int const rc = mutex_unlock(a0);
            if (g_signal_at_unlock and a0 == reinterpret_cast<uintptr_t>(&g_mutex.word))
            {
                g_signal_at_unlock = false;
                peer_signals();
            }
            return static_cast<uintptr_t>(rc);
        }
        case KOS_SYS_WORD_WAIT:
        {
            if (g_signal_at_wait and a0 == reinterpret_cast<uintptr_t>(&g_cond.word))
            {
                g_signal_at_wait = false;
                peer_signals();
            }
            return static_cast<uintptr_t>(
                word_wait(a0, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2)));
        }
        case KOS_SYS_WORD_WAKE:
        {
            return static_cast<uintptr_t>(
                word_wake(a0, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2)));
        }
        default:
        {
            ADD_FAILURE() << "the user library made syscall " << nr;
            return static_cast<uintptr_t>(-KOS_ENOSYS);
        }
    }
}

extern "C" uint32_t kickos_self_lock_id(void)
{
    return kickos::sched::current()->lock_id;
}

extern "C" uint64_t kos_clock_now(void)
{
    return kickos::testfix::g_now_ns;
}

extern "C" uint32_t __real_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired);

extern "C" uint32_t __wrap_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired)
{
    if (g_signal_at_cas and word == &g_cond.word)
    {
        g_signal_at_cas = false;
        peer_signals();
    }
    return __real_kickos_user_cas(word, expected, desired);
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO_BELOW = 3;
            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr int SLOT_FIRST = 2;
            constexpr int SLOT_LAST = 3;
            constexpr int SLOT_PEER = 5;
            constexpr uint32_t TIMEOUT_US = 1000;
            constexpr uint32_t PARKED = KOS_WORD_WAITERS;
#if KICKOS_KERNEL_CORES > 1
            constexpr uint32_t CORE_WAKER = KICKOS_KERNEL_CORES - 1;
            constexpr uint8_t PRIO_WAKER = 9;
            constexpr int SLOT_WAKER = 4;
#endif

            Thread* g_holder = nullptr;
            uint32_t g_cond_parks = 0;
            uint32_t g_mutex_parks = 0;
            bool g_holder_takes = false;

            uintptr_t va(uint32_t* word)
            {
                return reinterpret_cast<uintptr_t>(word);
            }

            WordRecord* record_of(uint32_t* word)
            {
                for (WordRecord* m = kernel().word_active; m != nullptr; m = m->next_active)
                {
                    if (m->key == static_cast<arch_phys_addr_t>(va(word)))
                    {
                        return m;
                    }
                }
                return nullptr;
            }

            bool on_cond(Thread const* parked)
            {
                return parked->wait_va == va(&g_cond.word);
            }

            // The holder runs as current in a waker, so its unlock passes the owner test.
            void holder_hands_back(Thread*)
            {
                g_mutex_parks++;
                EXPECT_EQ(mutex_unlock(va(&g_mutex.word)), 0);
            }

            // A park on the condition is answered by a signal, one on the mutex by the holder's
            // unlock.
            void answer(Thread* parked)
            {
                if (not on_cond(parked))
                {
                    holder_hands_back(parked);
                    return;
                }
                g_cond_parks++;
                wake_next_park(answer);
                peer_signals();
            }

            void deadline_fires()
            {
                g_now_ns += static_cast<uint64_t>(TIMEOUT_US) * 1000u + KICKOS_TIMER_MIN_DELTA_NS;
                ktime_on_timer();
            }

            void expire(Thread* parked)
            {
                if (not on_cond(parked))
                {
                    ADD_FAILURE() << "the timed wait parked on the mutex first";
                    return;
                }
                g_cond_parks++;
                if (g_holder_takes)
                {
                    EXPECT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
                    wake_next_park(holder_hands_back);
                }
                deadline_fires();
            }

            void broadcast(Thread* parked)
            {
                EXPECT_TRUE(on_cond(parked));
                g_cond_parks++;
                EXPECT_EQ(kos_cond_broadcast(&g_cond), 0);
            }

            void kill(Thread* parked)
            {
                EXPECT_TRUE(on_cond(parked));
                g_cond_parks++;
                thread_cancel(parked, kickos::IrqLock());
            }

            // The waiter is CURRENT, and the holder the only other runnable thread on its core,
            // so a park hands the CPU to the holder and a waker runs as it.
            Thread* seat_waiter_over_holder()
            {
                g_holder = seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
#if KICKOS_KERNEL_CORES > 1
                Thread* const waker = seat_pool(SLOT_WAKER, PRIO_WAKER);
                seat_running_on(waker, CORE_WAKER);
#endif
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(sched::current(), waiter) << "fixture: the waiter is current";
                g_cond = KOS_COND_INIT;
                g_mutex = KOS_MUTEX_INIT;
                g_spare = 0;
                g_signal_at_unlock = false;
                g_signal_at_cas = false;
                g_signal_at_wait = false;
                g_cond_parks = 0;
                g_mutex_parks = 0;
                g_holder_takes = false;
                return waiter;
            }

            // A waiter parked by hand on the condition, as a wait leaves it.
            Thread* parked_on_cond(int slot, uint8_t prio)
            {
                Thread* const t = seat_pool(slot, prio);
                WordRecord* m = record_of(&g_cond.word);
                if (m == nullptr)
                {
                    m = plain_word(&g_cond.word);
                }
                park_word_waiter(t, m);
                return t;
            }

            enum class Landing
            {
                RELEASE,
                WAITERS_BIT,
                PARK_ENTRY,
                PARK
            };

            struct Signal
            {
                char const* name;
                Landing at;
                uint32_t cond_parks;
            };

            class CondSignal : public KSeam, public ::testing::WithParamInterface<Signal>
            {
            };

            class CondWait : public KSeam
            {
            };

            void wait_then_syscall()
            {
                (void)kos_cond_wait(&g_cond, &g_mutex, KOS_TIMEOUT_NONE);
                (void)kos_word_wait(&g_spare, 1u, 0u);
            }
        }

        // A signal sent after the waiter released the mutex ends its wait wherever it lands:
        // from the thread the release handed the mutex to, at the waiters bit, at the park's
        // entry, or at the park.
        TEST_P(CondSignal, a_signal_after_the_release_ends_the_wait_with_the_mutex_retaken)
        {
            Signal const& s = GetParam();
            Thread* const waiter = seat_waiter_over_holder();
            if (s.at == Landing::RELEASE)
            {
                park_mutex_waiter(g_holder, own_mutex(waiter, &g_mutex.word));
                g_signal_at_unlock = true;
            }
            else
            {
                ASSERT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
            }
            g_signal_at_cas = (s.at == Landing::WAITERS_BIT);
            g_signal_at_wait = (s.at == Landing::PARK_ENTRY);
            wake_next_park(answer);

            EXPECT_EQ(kos_cond_wait(&g_cond, &g_mutex, KOS_TIMEOUT_NONE), 0);

            EXPECT_EQ(g_cond_parks, s.cond_parks)
                << "the waiter parked past a signal sent after its release";
            EXPECT_FALSE(g_signal_at_unlock or g_signal_at_cas or g_signal_at_wait)
                << "fixture: the signal never landed";
            EXPECT_EQ(sched::current(), waiter);
            EXPECT_EQ(g_mutex.word, waiter->lock_id) << "the wait returned without the mutex";
            EXPECT_EQ(g_cond.word, 1u) << "the signal did not advance the sequence once";
            EXPECT_EQ(record_of(&g_cond.word), nullptr) << "the emptied record was kept";
        }

        INSTANTIATE_TEST_SUITE_P(
            Landing, CondSignal,
            ::testing::Values(Signal{"from_the_thread_the_release_handed_the_mutex",
                                     Landing::RELEASE, 0u},
                              Signal{"at_the_waiters_bit", Landing::WAITERS_BIT, 0u},
                              Signal{"at_the_park_entry", Landing::PARK_ENTRY, 0u},
                              Signal{"at_the_park", Landing::PARK, 1u}),
            [](::testing::TestParamInfo<Signal> const& p)
            {
                return std::string{p.param.name};
            });

        TEST_F(CondWait, a_broadcast_wakes_every_waiter)
        {
            Thread* const waiter = seat_waiter_over_holder();
            Thread* const first = parked_on_cond(SLOT_FIRST, PRIO_BELOW);
            Thread* const last = parked_on_cond(SLOT_LAST, PRIO_BELOW);
            ASSERT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
            wake_next_park(broadcast);

            EXPECT_EQ(kos_cond_wait(&g_cond, &g_mutex, KOS_TIMEOUT_NONE), 0);

            EXPECT_EQ(g_cond_parks, 1u);
            EXPECT_NE(first->state, ThreadState::BLOCKED) << "the broadcast left a waiter parked";
            EXPECT_NE(last->state, ThreadState::BLOCKED) << "the broadcast left a waiter parked";
            EXPECT_EQ(first->wait_result, 0);
            EXPECT_EQ(last->wait_result, 0);
            EXPECT_EQ(g_mutex.word, waiter->lock_id);
            EXPECT_EQ(g_cond.word, 1u);
            EXPECT_EQ(record_of(&g_cond.word), nullptr);
        }

        TEST_F(CondWait, a_signal_wakes_the_highest_waiter_alone)
        {
            seat_waiter_over_holder();
            Thread* const low = parked_on_cond(SLOT_FIRST, PRIO_BELOW);
            Thread* const high = parked_on_cond(SLOT_LAST, PRIO_HOLDER);

            EXPECT_EQ(kos_cond_signal(&g_cond), 0);
            EXPECT_NE(high->state, ThreadState::BLOCKED);
            EXPECT_EQ(low->state, ThreadState::BLOCKED) << "a signal woke more than one waiter";
            EXPECT_EQ(g_cond.word, PARKED | 1u);

            EXPECT_EQ(kos_cond_signal(&g_cond), 0);
            EXPECT_NE(low->state, ThreadState::BLOCKED);
            EXPECT_EQ(g_cond.word, 2u);
            EXPECT_EQ(record_of(&g_cond.word), nullptr);

            EXPECT_EQ(kos_cond_broadcast(&g_cond), 0) << "an unwaited broadcast entered the kernel";
            EXPECT_EQ(g_cond.word, 3u);
        }

        namespace
        {
            // The waiter's bit stays for the next signal to clear.
            void timed_wait_ends_at_its_deadline(bool held)
            {
                Thread* const waiter = seat_waiter_over_holder();
                g_holder_takes = held;
                uint32_t mutex_parks = 0;
                if (held)
                {
                    mutex_parks = 1;
                }
                ASSERT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
                uint64_t const start_ns = g_now_ns;
                wake_next_park(expire);

                EXPECT_EQ(kos_cond_wait(&g_cond, &g_mutex, TIMEOUT_US), -KOS_ETIMEDOUT);

                EXPECT_EQ(g_cond_parks, 1u);
                EXPECT_EQ(g_mutex_parks, mutex_parks);
                EXPECT_GE(g_now_ns - start_ns, static_cast<uint64_t>(TIMEOUT_US) * 1000u);
                EXPECT_EQ(sched::current(), waiter);
                EXPECT_EQ(g_mutex.word, waiter->lock_id) << "the wait returned without the mutex";
                EXPECT_EQ(g_cond.word, PARKED);
                EXPECT_EQ(record_of(&g_cond.word), nullptr);
                EXPECT_EQ(kernel().sleepq(kickos_kernel_core()), nullptr) << "the waiter's deadline is still queued";
                EXPECT_EQ(kos_cond_signal(&g_cond), 0);
                EXPECT_EQ(g_cond.word, 1u) << "the signal left the dead bit set";
            }
        }

        TEST_F(CondWait, a_timed_wait_ends_at_its_deadline_with_the_mutex_retaken)
        {
            timed_wait_ends_at_its_deadline(false);
        }

        TEST_F(CondWait, a_timed_wait_ends_at_its_deadline_and_waits_out_the_mutex_holder)
        {
            timed_wait_ends_at_its_deadline(true);
        }

        TEST_F(CondWait, a_waiter_not_holding_the_mutex_is_refused_with_nothing_waited)
        {
            seat_waiter_over_holder();
            EXPECT_EQ(kos_cond_wait(&g_cond, &g_mutex, KOS_TIMEOUT_NONE), -KOS_EPERM);
            EXPECT_EQ(g_mutex.word, 0u);
            EXPECT_EQ(g_cond.word, 0u);
        }

        // A waiter killed while parked dies at the re-take's kernel entry, never holding the
        // mutex, and the next wait and signal on the condition work.
        TEST_F(CondWait, a_waiter_killed_while_parked_leaves_the_condition_usable)
        {
            Thread* const waiter = seat_waiter_over_holder();
            ASSERT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
            wake_next_park(kill);

            run_noreturn(wait_then_syscall);

            EXPECT_EQ(g_cond_parks, 1u);
            EXPECT_EQ(waiter->state, ThreadState::EXITED);
            EXPECT_EQ(g_mutex.word, 0u) << "the killed waiter took the mutex on its way out";
            EXPECT_EQ(record_of(&g_cond.word), nullptr);

            (void)seat_pool(SLOT_PEER, PRIO_BELOW);
            ASSERT_EQ(sched::current(), g_holder);
            g_cond_parks = 0;
            ASSERT_EQ(kos_mutex_lock(&g_mutex, 0), 0);
            wake_next_park(answer);
            EXPECT_EQ(kos_cond_wait(&g_cond, &g_mutex, KOS_TIMEOUT_NONE), 0);
            EXPECT_EQ(g_cond_parks, 1u);
            EXPECT_EQ(g_mutex.word, g_holder->lock_id);
            EXPECT_EQ(g_cond.word, 1u);
            EXPECT_EQ(record_of(&g_cond.word), nullptr);
        }
    }
}
