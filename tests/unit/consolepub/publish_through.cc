// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cap_console_publish_through: HANDOUT is required, the console's registers may be held by no
// thread outside the task named, a publisher not holding WAIT is seated as a receiver and the
// endpoint's vacated mark cleared, and a refusal changes nothing.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/task.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class ConsolePublish : public KSeam
            {
            };

            constexpr uint8_t PRIO_PUBLISHER = 5;
            constexpr uint32_t PUBLISH_INDEX = KICKOS_CAP_FIRST_DYNAMIC;

            struct Published
            {
                Thread* publisher;
                Endpoint* ep;
                int handle;
                CapEntry* e;
            };

            // A publisher holding one capability of `rights` on a fresh endpoint, through the real
            // counter locator.
            Published hold(uint8_t rights)
            {
                Published p{};
                p.publisher = spawn(0, PRIO_PUBLISHER);
                attach_caps(p.publisher, PUBLISH_INDEX + 1);
                p.ep = endpoint();
                p.handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(p.ep));
                p.e = cap_install_at(p.publisher, PUBLISH_INDEX, p.handle, CapType::CAP_ENDPOINT,
                                     rights, KCAP_BADGE_NONE);
                EXPECT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, p.handle, rights))
                    << "fixture: the cap took its references";
                return p;
            }

            int publish(Published const& p)
            {
                IrqLock lock;
                return cap_console_publish_through(p.publisher, p.e, p.publisher->task);
            }

            // Everything a refusal must leave as it found it.
            void expect_unchanged(Published const& p, uint8_t rights, uint8_t holders,
                                  uint8_t vacated)
            {
                EXPECT_EQ(p.e->rights, rights) << "the publisher's rights moved";
                EXPECT_EQ(p.ep->recv_holders, holders) << "the receiver count moved";
                EXPECT_EQ(p.ep->vacated, vacated) << "the vacated mark moved";
                EXPECT_EQ(cap_console_endpoint(), nullptr) << "a console was published";
                EXPECT_EQ(cap_slot(p.publisher->caps, 0)->type, static_cast<uint8_t>(CapType::CAP_EMPTY))
                    << "the publisher's stdout was seated";
            }
        }

        TEST_F(ConsolePublish, a_capability_without_handout_is_refused)
        {
            uint8_t const rights = CAP_SIGNAL | CAP_TRANSFER;
            Published const p = hold(rights);
            p.ep->vacated = 1;

            EXPECT_EQ(publish(p), -KOS_EACCES);
            expect_unchanged(p, rights, 0, 1);
        }

        TEST_F(ConsolePublish, a_wait_only_capability_is_refused)
        {
            uint8_t const rights = CAP_WAIT;
            Published const p = hold(rights);
            p.ep->recv_holders = 1;

            EXPECT_EQ(publish(p), -KOS_EACCES);
            expect_unchanged(p, rights, 1, 0);
        }

        TEST_F(ConsolePublish, a_seat_past_the_receiver_ceiling_is_refused)
        {
            uint8_t const rights = CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT;
            Published const p = hold(rights);
            p.ep->recv_holders = UINT8_MAX;
            p.ep->vacated = 1;

            EXPECT_EQ(publish(p), -KOS_EOVERFLOW);
            expect_unchanged(p, rights, UINT8_MAX, 1);
        }

        // The console's registers held by a thread outside the task named: that task's end would
        // not free them, so no reclaim would follow its death.
        TEST_F(ConsolePublish, a_task_not_holding_the_console_window_is_refused)
        {
            uint8_t const rights = CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT;
            Published const p = hold(rights);
            p.ep->vacated = 1;
            g_console_window_held = true;

            EXPECT_EQ(publish(p), -KOS_EBUSY);
            expect_unchanged(p, rights, 0, 1);
        }

        TEST_F(ConsolePublish, a_handout_only_publisher_is_seated_as_a_receiver)
        {
            Published const p = hold(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            p.ep->vacated = 1;

            ASSERT_EQ(publish(p), 0);

            EXPECT_EQ(p.e->rights, CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT)
                << "the publisher holds WAIT and HANDOUT";
            EXPECT_EQ(p.ep->recv_holders, 1u) << "and counts as receiving";
            EXPECT_EQ(p.ep->vacated, 0u) << "so the endpoint is no longer vacated";
            EXPECT_EQ(cap_console_endpoint(), p.ep) << "the endpoint is the console";
            EXPECT_EQ(cap_slot(p.publisher->caps, 0)->type, static_cast<uint8_t>(CapType::CAP_ENDPOINT))
                << "and the publisher's stdout names it";
        }

        TEST_F(ConsolePublish, a_receiving_publisher_is_not_counted_twice)
        {
            Published const p = hold(CAP_RIGHTS_ALL);
            p.ep->recv_holders = 1;

            ASSERT_EQ(publish(p), 0);

            EXPECT_EQ(p.e->rights, CAP_RIGHTS_ALL);
            EXPECT_EQ(p.ep->recv_holders, 1u) << "a WAIT already held is not seated again";
            EXPECT_EQ(cap_console_endpoint(), p.ep);
        }

        bool serves(Thread const* t)
        {
            IrqLock lock;
            return cap_console_serves(t);
        }

        // cap_console_serves answers for the thread's own stdout slot and whether the task the
        // console was published for lives, and for nothing else: a vacated endpoint still serves.
        TEST_F(ConsolePublish, only_the_published_stdout_of_a_live_task_serves)
        {
            Published const p = hold(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            Task* const served = task(0);
            EXPECT_FALSE(serves(p.publisher)) << "nothing is published yet";
            {
                IrqLock lock;
                ASSERT_EQ(cap_console_publish_through(p.publisher, p.e, served), 0);
            }
            EXPECT_TRUE(serves(p.publisher));

            Thread* const runless = spawn(1, PRIO_PUBLISHER);
            EXPECT_FALSE(serves(runless)) << "a thread with no table";

            Thread* const empty = spawn(2, PRIO_PUBLISHER);
            attach_caps(empty, PUBLISH_INDEX + 1);
            EXPECT_FALSE(serves(empty)) << "an empty stdout slot";

            CapEntry& out = *cap_slot(p.publisher->caps, KOS_CAP_STDOUT);
            uint8_t const rights = out.rights;
            out.rights = 0;
            EXPECT_FALSE(serves(p.publisher)) << "a slot without SIGNAL";
            out.rights = rights;

            int const obj = out.obj;
            out.obj = kernel().endpoints.handle_for(kernel().endpoints.index_of(endpoint()));
            EXPECT_FALSE(serves(p.publisher)) << "a slot naming another endpoint";
            out.obj = obj;

            p.ep->vacated = 1;
            EXPECT_TRUE(serves(p.publisher)) << "a vacated endpoint of a live task";
            {
                IrqLock lock;
                task_end(served, 0, true);
            }
            EXPECT_FALSE(serves(p.publisher)) << "the task ended";
            p.ep->vacated = 0;
            EXPECT_FALSE(serves(p.publisher)) << "the task's receivers are dying";
        }
    }
}
