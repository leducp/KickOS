// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-core delta lists at two kernel cores, over the real time.cc, sync.cc and sched.cc: a
// deadline goes on the queue of the core its thread parked on, only that core's comparator is
// armed for it and only that core's timer expires it; a removal from the peer leaves the owner
// one fire that wakes nothing; and a set of the wall clock on the peer that moves a deadline
// earlier asks the owner, whose dispatch re-arms.

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

#include <string>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO_SECOND = 2;
            constexpr uint8_t PRIO_HOLDER = 5;
            constexpr uint8_t PRIO_WAITER = 6;
            constexpr uint8_t PRIO_PEER = 9;
            constexpr int SLOT_HOLDER = 0;
            constexpr int SLOT_WAITER = 1;
            constexpr int SLOT_SECOND = 2;
            constexpr int SLOT_PEER = 4;
            constexpr uint32_t OWNER = 0;
            constexpr uint32_t PEER = 1;
            constexpr uint32_t TIMEOUT_US = 1000;
            constexpr uint64_t MS_NS = 1000000u;
            constexpr uint64_t HOUR_NS = 3600000000000ull;
            constexpr uint64_t BASE_NS = 1700000000000000000ull;

            uint32_t g_word = 0;
            Thread* g_peer = nullptr;

            uintptr_t word_va()
            {
                return reinterpret_cast<uintptr_t>(&g_word);
            }

            // Runs its scope as `core`.
            class AsCore
            {
              public:
                explicit AsCore(uint32_t core)
                    : was_(g_core)
                {
                    g_core = core;
                }
                ~AsCore()
                {
                    g_core = was_;
                }
                AsCore(AsCore const&) = delete;
                AsCore& operator=(AsCore const&) = delete;

              private:
                uint32_t was_;
            };

            Thread* head(uint32_t core)
            {
                return kernel().sleepq(core_at(core));
            }

            // The waiter is CURRENT on the owner over the holder, and the peer runs above both on
            // the other core, so nothing is placed there.
            Thread* seat_waiter_with_peer()
            {
                (void)seat_pool(SLOT_HOLDER, PRIO_HOLDER);
                Thread* const waiter = seat_pool(SLOT_WAITER, PRIO_WAITER);
                g_peer = seat_pool(SLOT_PEER, PRIO_PEER);
                g_peer->authority = AUTH_REALTIME;
                seat_running_on(g_peer, PEER);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(core_at(OWNER)), waiter)
                    << "fixture: the waiter is current on the owner";
                g_word = 0;
                return waiter;
            }

            int timed_wait(WaitLimit limit)
            {
                g_word = g_word | KOS_WORD_WAITERS;
                return word_wait(word_va(), g_word, limit);
            }

            void fire(uint32_t core)
            {
                AsCore const as(core);
                ktime_on_timer();
            }

            // What the arm's waker saw, read by the arm once the wait has returned.
            uint64_t g_deadline = 0;
            uint64_t g_deadline_after = 0;
            bool g_on_owner_queue = false;
            bool g_peer_queue_empty = false;
            uint64_t g_owner_armed = 0;
            uint64_t g_owner_armed_after = 0;
            uint64_t g_owner_armed_last = 0;
            uint64_t g_peer_armed = 0;
            bool g_blocked_after_peer_fire = false;

            void peer_rearms_then_fires_first(Thread* parked)
            {
                g_deadline = parked->deadline_ns;
                g_on_owner_queue = head(OWNER) == parked;
                g_peer_queue_empty = head(PEER) == nullptr;
                g_owner_armed = g_comparator[OWNER];
                {
                    AsCore const peer(PEER);
                    IrqLock lock;
                    ktime_rearm(kernel().current(core_at(PEER)), lock);
                }
                g_peer_armed = g_comparator[PEER];
                g_now_ns = g_deadline;
                fire(PEER);
                g_blocked_after_peer_fire = parked->state == ThreadState::BLOCKED;
                fire(OWNER);
            }

            ThreadState g_state_after = ThreadState::INACTIVE;
            uint8_t g_queue_core_after = 0;
            bool g_on_timer_after = true;
            bool g_queues_empty_after = false;
            bool g_peer_asked = false;

            void narrow_to_peer_then_expire(Thread* parked)
            {
                g_deadline = parked->deadline_ns;
                sched::set_affinity(parked, 1u << PEER);
                g_ipi_send_mask = 0;
                g_now_ns = g_deadline;
                fire(OWNER);
                g_state_after = parked->state;
                g_queue_core_after = parked->queue_core;
                g_on_timer_after = parked->on_timer;
                g_queues_empty_after = head(OWNER) == nullptr and head(PEER) == nullptr;
                g_peer_armed = g_comparator[PEER];
                g_peer_asked = (g_ipi_send_mask & (1u << PEER)) != 0;
            }

            // A sleeper parked on the owner behind the arm's waiter, by the owner.
            Thread* g_second = nullptr;
            uint64_t g_second_deadline = 0;
            bool g_second_next = false;
            bool g_second_blocked_after_fire = false;
            bool g_removed_from_queue = false;

            void queue_second()
            {
                g_second = seat_pool(SLOT_SECOND, PRIO_SECOND);
                detach_ready(g_second);
                seat_blocked(g_second);
                g_second->wait_queue = nullptr;
                g_second->wait_kind = WAIT_SLEEP;
                g_second->wait_obj = nullptr;
                g_second->wait_result = WAIT_RESULT_POISON;
                IrqLock lock;
                ktime_deadline_arm(g_second, 2u * TIMEOUT_US, lock);
                g_second_deadline = g_second->deadline_ns;
            }

            void (*g_remove)(Thread* parked) = nullptr;

            void post(Thread*)
            {
                EXPECT_EQ(word_wake(word_va(), 1u, 1u), 1);
            }

            void kill(Thread* parked)
            {
                thread_cancel(parked, IrqLock());
            }

            void slay(Thread* parked)
            {
                thread_cancel_kind(parked, CANCEL_SLAY, IrqLock());
            }

            void remove_from_peer_then_fire(Thread* parked)
            {
                g_deadline = parked->deadline_ns;
                queue_second();
                {
                    AsCore const peer(PEER);
                    g_remove(parked);
                }
                g_removed_from_queue = not parked->on_timer;
                g_second_next = head(OWNER) == g_second and g_second->tnext == nullptr;
                g_owner_armed = g_comparator[OWNER];
                g_now_ns = g_deadline;
                fire(OWNER);
                g_second_blocked_after_fire = g_second->state == ThreadState::BLOCKED;
                g_owner_armed_after = g_comparator[OWNER];
                g_now_ns = g_second_deadline;
                fire(OWNER);
                g_owner_armed_last = g_comparator[OWNER];
            }

            int g_owed_after_set = 0;

            void set_wall(uint64_t unix_ns)
            {
                ASSERT_EQ(realtime_set(g_peer, unix_ns), 0);
            }

            void peer_pulls_the_wall_deadline_in(Thread* parked)
            {
                g_deadline = parked->deadline_ns;
                g_owner_armed = g_comparator[OWNER];
                drain(OWNER);
                {
                    AsCore const peer(PEER);
                    set_wall(BASE_NS + HOUR_NS - 10u * MS_NS);
                }
                g_deadline_after = parked->deadline_ns;
                g_owner_armed_after = g_comparator[OWNER];
                g_peer_armed = g_comparator[PEER];
                g_owed_after_set = owed_at(OWNER);
                dispatch_as(OWNER);
                g_owner_armed_last = g_comparator[OWNER];
                g_now_ns = g_deadline_after;
                fire(OWNER);
            }

            struct Removal
            {
                char const* name;
                void (*remove)(Thread* parked);
                int rc;
            };

            class SleepqCores : public KSeam
            {
            };

            class SleepqRemoval : public KSeam, public ::testing::WithParamInterface<Removal>
            {
            };
        }

        TEST_F(SleepqCores, a_peer_neither_arms_for_nor_expires_another_cores_deadline)
        {
            seat_waiter_with_peer();
            wake_next_park(peer_rearms_then_fires_first);

            EXPECT_EQ(timed_wait(TIMEOUT_US), -KOS_ETIMEDOUT);

            EXPECT_TRUE(g_on_owner_queue) << "the deadline is not on the queue of its park core";
            EXPECT_TRUE(g_peer_queue_empty) << "the peer's queue holds the owner's deadline";
            EXPECT_EQ(g_owner_armed, g_deadline) << "the park's own switch did not arm it";
            EXPECT_EQ(g_peer_armed, UINT64_MAX)
                << "a peer's switch armed its comparator for another core's sleeper";
            EXPECT_TRUE(g_blocked_after_peer_fire) << "a peer's timer expired another core's deadline";
        }

        TEST_F(SleepqCores, an_expiry_on_the_park_core_hands_a_sleeper_its_mask_moved_off_it)
        {
            seat_waiter_with_peer();
            wake_next_park(narrow_to_peer_then_expire);

            EXPECT_EQ(timed_wait(TIMEOUT_US), -KOS_ETIMEDOUT);

            EXPECT_EQ(g_state_after, ThreadState::HANDED);
            EXPECT_EQ(g_queue_core_after, PEER);
            EXPECT_FALSE(g_on_timer_after) << "the deadline outlived the expiry";
            EXPECT_TRUE(g_queues_empty_after);
            EXPECT_TRUE(g_peer_asked) << "the expiry handed the sleeper and asked nobody";
            EXPECT_EQ(g_peer_armed, 0u) << "the hand-off reprogrammed the peer's comparator";
        }

        TEST_P(SleepqRemoval, leaves_the_owner_one_fire_that_wakes_nothing_and_rearms_to_its_next)
        {
            Removal const& r = GetParam();
            seat_waiter_with_peer();
            g_remove = r.remove;
            wake_next_park(remove_from_peer_then_fire);

            EXPECT_EQ(timed_wait(TIMEOUT_US), r.rc);

            EXPECT_TRUE(g_removed_from_queue) << "the deadline outlived its removal";
            EXPECT_TRUE(g_second_next) << "the owner's queue does not hold its next deadline alone";
            EXPECT_EQ(g_owner_armed, g_deadline) << "a removal on the peer reprogrammed the owner";
            EXPECT_TRUE(g_second_blocked_after_fire) << "the empty fire woke the next sleeper";
            EXPECT_EQ(g_owner_armed_after, g_second_deadline)
                << "the empty fire did not re-arm for the owner's next deadline";
            EXPECT_EQ(g_second->wait_result, -KOS_ETIMEDOUT);
            EXPECT_EQ(g_owner_armed_last, UINT64_MAX) << "an empty queue left the owner armed";
            EXPECT_EQ(head(OWNER), nullptr);
            EXPECT_EQ(head(PEER), nullptr);
        }

        INSTANTIATE_TEST_SUITE_P(
            FromPeer, SleepqRemoval,
            ::testing::Values(Removal{"a_post", post, 1},
                              Removal{"a_kill", kill, -KOS_ECANCELED},
                              Removal{"a_slay", slay, -KOS_ECANCELED}),
            [](::testing::TestParamInfo<Removal> const& p) {
                return std::string(p.param.name);
            });

        TEST_F(SleepqCores, a_peer_moving_a_deadline_earlier_asks_its_owner_whose_dispatch_rearms)
        {
            seat_waiter_with_peer();
            set_wall(BASE_NS);
            wake_next_park(peer_pulls_the_wall_deadline_in);

            EXPECT_EQ(timed_wait(WaitLimit::realtime(BASE_NS + HOUR_NS)), -KOS_ETIMEDOUT);

            EXPECT_LT(g_deadline_after, g_deadline) << "fixture: the set moved nothing earlier";
            EXPECT_EQ(g_owner_armed, g_deadline);
            EXPECT_EQ(g_owner_armed_after, g_deadline) << "the peer reprogrammed the owner itself";
            EXPECT_EQ(g_peer_armed, 0u) << "the peer armed for the owner's deadline";
            EXPECT_NE(g_owed_after_set, 0) << "the peer moved the owner's deadline and asked nothing";
            EXPECT_EQ(g_owner_armed_last, g_deadline_after)
                << "the owner's dispatch left its comparator on the old deadline";
        }

        // Each queue edit asserts it is keyed where the thread's queue_core says, the one fact
        // a removal from another core can find the queue by.
        TEST_F(SleepqCores, a_deadline_looked_up_on_a_queue_not_holding_it_panics)
        {
            seat_waiter_with_peer();
            KICKOS_EXPECT_PANIC(
                {
                    queue_second();
                    g_second->queue_core = PEER;
                    IrqLock lock;
                    ktime_deadline_cancel(g_second, lock);
                },
                "debug assert: \\*pp == t");
        }

        TEST_F(SleepqCores, a_deadline_armed_for_a_thread_of_another_core_panics)
        {
            seat_waiter_with_peer();
            KICKOS_EXPECT_PANIC(
                {
                    AsCore const peer(PEER);
                    IrqLock lock;
                    ktime_deadline_arm(kernel().current(core_at(OWNER)), TIMEOUT_US, lock);
                },
                "debug assert: queue_core_of\\(t\\) == kickos_kernel_core\\(\\)");
        }
    }
}
