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
            constexpr int SLOT_OTHER = 2;
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

        // --- The retraction of an UNDISCLOSED mint -------------------------------------
        // Both local mint sites write the handle into the receiver's memory AFTER installing
        // it, and that write can be refused. A capability nobody was told the handle of can
        // never be spent, so the install is undone. What has to come back is not just the
        // table SLOT but the reply BOUND, and only one of those is visible in the free list:
        // an undo that emptied the entry without cap_reply_released leaves the bound spent
        // and the receiver's next caller refused against a capability that is already gone.
        TEST_F(CapProbe, an_undisclosed_mint_gives_back_the_slot_and_the_bound)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            attach_caps(server, TABLE_WIDTH);

            // REPEATED TO THE BOUND: one undo that frees the slot alone still leaves the free
            // list non-empty, so a single pass cannot tell the two halves apart.
            for (uint32_t i = 0; i < REPLY_MAX + 1u; i++)
            {
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(cap_install_reply(server, client, &cap), 0)
                    << "mint " << i << " runs against a bound every undo before it returned";
                ASSERT_NE(cap, KCAP_INVALID);
                EXPECT_EQ(cap_reply_live(server), 1u) << "exactly this one mint is live";
                EXPECT_TRUE(cap_uninstall_reply(server, cap, client))
                    << "the undo names the reply it minted";
                EXPECT_EQ(cap_reply_live(server), 0u) << "and the bound came back with it";
            }
            // The slot is free too, so the table is where it started.
            EXPECT_TRUE(cap_can_take_reply(server));
        }

        // TOTAL over a capability that is not the one it was asked to retract, which is what
        // lets the two call sites assert their own answer. Nothing between a mint and its undo
        // can reach these states; they are the reason the undo is a bool and not a void.
        TEST_F(CapProbe, the_retraction_refuses_a_capability_it_did_not_mint)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            Thread* const other = seat_pool(SLOT_OTHER, PRIO);
            attach_caps(server, TABLE_WIDTH);

            // ITS obj IS THE CALLER'S OWN THREAD HANDLE, so the identity test below cannot
            // be what refuses it and the TYPE tag is: cap_install takes any obj it is given,
            // and cap_reply_handle reads that word whole.
            int const client_obj = static_cast<int>(
                kernel().threads.handle_for(kernel().threads.index_of(client)));
            uint32_t collide = KCAP_INVALID;
            ASSERT_EQ(cap_install(server, client_obj, CapType::CAP_SEM, CAP_WAIT, &collide), 0);
            EXPECT_FALSE(cap_uninstall_reply(server, collide, client))
                << "a capability of another type is not a reply to retract, whatever its obj";

            uint32_t cap = KCAP_INVALID;
            ASSERT_EQ(cap_install_reply(server, client, &cap), 0);
            EXPECT_FALSE(cap_uninstall_reply(server, cap, other))
                << "a reply naming another caller is not this caller's to retract";
            EXPECT_EQ(cap_reply_live(server), 1u) << "and a refused undo retracts nothing";
            EXPECT_TRUE(cap_uninstall_reply(server, cap, client));
        }

        // THE RETRACTED HANDLE AGAINST THE SLOT'S NEXT OCCUPANT. The undo bumps the entry's
        // cap-gen exactly as handle_close does; without that bump the retracted word still
        // names the slot and would retract whatever reply lands there next, which is the ABA
        // the gen exists for.
        //
        // THE SLOT HAS TO BE THE SAME SLOT, and that is not the default: a released entry goes
        // to the free list's TAIL while a mint takes its HEAD, so an ordinary retract-then-mint
        // pair lands on a different index and the two handles differ for a reason that has
        // nothing to do with the generation. The table is filled around the one slot under
        // test so the list holds exactly it, and the arm ASSERTS the index came back.
        TEST_F(CapProbe, a_retracted_handle_cannot_reach_the_slots_next_occupant)
        {
            Thread* const server = seat_pool(SLOT_SERVER, PRIO);
            Thread* const client = seat_pool(SLOT_CLIENT, PRIO);
            attach_caps(server, TABLE_WIDTH);

            uint32_t cap = KCAP_INVALID;
            ASSERT_EQ(cap_install_reply(server, client, &cap), 0);
            int sem_handle = 0;
            (void)semaphore(&sem_handle);
            while (server->cap_free_head != KCAP_FREE_NONE)
            {
                uint32_t filler = KCAP_INVALID;
                ASSERT_EQ(cap_install(server, sem_handle, CapType::CAP_SEM, CAP_WAIT, &filler), 0);
            }
            ASSERT_TRUE(cap_uninstall_reply(server, cap, client));
            ASSERT_NE(server->cap_free_head, KCAP_FREE_NONE) << "the undo gave the slot back";

            uint32_t again = KCAP_INVALID;
            ASSERT_EQ(cap_install_reply(server, client, &again), 0);
            ASSERT_EQ(again & KCAP_INDEX_MASK, cap & KCAP_INDEX_MASK)
                << "the premise: the mint took the retracted slot, so only the generation can "
                   "tell the two handles apart";
            EXPECT_NE(again, cap) << "and it does tell them apart";
            EXPECT_FALSE(cap_uninstall_reply(server, cap, client))
                << "the retracted handle names nothing, not this slot's next occupant";
            EXPECT_EQ(cap_reply_live(server), 1u) << "so that occupant is still live";
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
