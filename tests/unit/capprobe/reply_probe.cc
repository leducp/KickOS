// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cap_can_take_reply is the ENTIRE check behind the KICKOS_ASSERT(minted == 0) that follows
// every reply mint (two in kernel/syscall/syscall_ipc.cc, one in syscall_ipc_fast.cc), and
// KICKOS_ASSERT panics in every posture: the probe must refuse in exactly the states
// cap_install_reply refuses in.
//
// Each refusing arm builds ONE of the two refusal states and asserts the other clause is
// still satisfied, so a probe that lost either clause fails an arm.

#include <kickos/cap.h>
#include <kickos/instance.h>
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
            class CapProbe : public KSeam
            {
            };

            constexpr int SLOT_SERVER = 0;
            constexpr int SLOT_CLIENT = 1;
            constexpr uint8_t PRIO = 5;

            // The gtest comparison helpers bind by reference, so the macro's signed literal
            // reaches -Wsign-compare as a non-constant.
            constexpr uint32_t REPLY_MAX = KICKOS_CAP_REPLY_MAX;

            // Two spare dynamic slots past the reply bound, so the bound is reachable with
            // the free list still non-empty; without the slack no arm can isolate one clause.
            constexpr uint32_t TABLE_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + REPLY_MAX + 2;
            static_assert(TABLE_WIDTH <= KICKOS_MAX_HANDLES,
                          "the arms below ask the slab for a table wider than the codec's "
                          "ceiling, which cap_slab_attach asserts on");

            // THE CLAIM: the probe's answer and the mint's are one answer. Consumes a slot
            // when it succeeds, so an arm calls it last.
            void expect_probe_answers_mint(Thread* server, Thread* client, bool expected)
            {
                bool const probe = cap_can_take_reply(server);
                EXPECT_EQ(probe, expected) << "the probe read the state this arm built";
                uint32_t cap = KCAP_INVALID;
                int const minted = cap_install_reply(server, client, &cap);
                EXPECT_EQ(probe, minted == 0)
                    << "the probe IS the mint's precondition: KICKOS_ASSERT(minted == 0) at "
                       "all three call sites has nothing else behind it";
                if (not expected)
                {
                    EXPECT_EQ(minted, -KOS_EMFILE)
                        << "a refused mint reports the table's own error, never a panic";
                    EXPECT_EQ(cap, KCAP_INVALID)
                        << "and leaves the out-parameter at the sentinel";
                }
            }
        }

        // The baseline, so a later arm's refusal is the state that arm built, not the
        // fixture's.
        TEST_F(CapProbe, a_fresh_table_takes_a_reply)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            attach_caps(server, TABLE_WIDTH);

            expect_probe_answers_mint(server, client, true);
        }

        // The REPLY-BOUND clause alone: dynamic slots are still free, so cap_reply_live is the
        // only thing that can refuse here.
        TEST_F(CapProbe, the_reply_bound_refuses_while_slots_are_still_free)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            attach_caps(server, TABLE_WIDTH);
            for (uint32_t i = 0; i < REPLY_MAX; i++)
            {
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(cap_install_reply(server, client, &cap), 0)
                    << "filling to the bound is not itself a refusal";
            }
            ASSERT_EQ(cap_reply_live(server), REPLY_MAX) << "seated at the bound";
            ASSERT_NE(server->cap_free_head, KCAP_FREE_NONE)
                << "with the free list still non-empty, so this arm is about the bound only";

            expect_probe_answers_mint(server, client, false);
        }

        // The FREE-LIST clause alone: no reply cap is live, so the emptied free list is the
        // only thing that can refuse here.
        TEST_F(CapProbe, a_full_table_refuses_with_the_reply_bound_untouched)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            attach_caps(server, TABLE_WIDTH);
            int sem_handle = 0;
            (void)semaphore(&sem_handle);
            // cap_install takes no reference on the object it names.
            while (server->cap_free_head != KCAP_FREE_NONE)
            {
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(cap_install(server, sem_handle, CapType::CAP_SEM, CAP_WAIT, &cap), 0)
                    << "the free list and cap_install must agree about what is left";
            }
            ASSERT_EQ(cap_reply_live(server), 0u)
                << "no reply cap is live, so this arm is about the free list only";

            expect_probe_answers_mint(server, client, false);
        }
    }
}
