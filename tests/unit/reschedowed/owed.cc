// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The reschedule a cross-core raise carries, held as state rather than as the raise. An arm
// here IS the peer at the instant it chooses: it speaks as the initiator to owe a reschedule,
// then as the target to run the lock bracket that must restore the raise. What absorbed the
// original raise is a controller register out of reach on the host, so these arms fix the half
// that decides whether an absorbed raise costs a reschedule: the cell, the release that reads
// it, and the one body that clears it.
//
// THE ASK'S OWN ORDERING IS WITNESSED HERE TOO, and it is witnessable only because the raise is
// a seam call: the stub samples the cell FROM THE TARGET'S SEAT, so an ask published behind its
// own raise is a zero in g_owed_at_send rather than an interleaving no host fixture can force.

#include "resched_seam.h"

#include <kickos/arch/arch.h>
#include <kickos/klock.h>

#include <gtest/gtest.h>

namespace
{
    namespace fix = kickos::reschedfix;

    constexpr uint32_t CORE_A = 0; // the initiator
    constexpr uint32_t CORE_B = 1; // the target owed a reschedule

    // Owe `targets` a reschedule while speaking as core `from`.
    void owe_as(uint32_t from, uint32_t targets)
    {
        uint32_t const was = fix::g_core;
        fix::g_core = from;
        kickos_kernel_core_resched_owe(targets);
        fix::g_core = was;
    }

    // One whole lock bracket on core `core`, entered and left at depth one.
    void bracket_as(uint32_t core)
    {
        uint32_t const was = fix::g_core;
        fix::g_core = core;
        kickos::klock_enter();
        kickos::klock_leave();
        fix::g_core = was;
    }

    // The scheduler's own cross-core ask, spoken as core `from`.
    void ask_as(uint32_t from, uint32_t targets)
    {
        uint32_t const was = fix::g_core;
        fix::g_core = from;
        kickos::klock_resched_ask(targets);
        fix::g_core = was;
    }

    // What the interrupt dispatch does: consume the cell, then enter the scheduler.
    int dispatch_as(uint32_t core)
    {
        uint32_t const was = fix::g_core;
        fix::g_core = core;
        int const stood = kickos_kernel_core_resched_take();
        fix::g_core = was;
        return stood;
    }

    int owed_as(uint32_t core)
    {
        uint32_t const was = fix::g_core;
        fix::g_core = core;
        int const owed = kickos_kernel_core_resched_owed();
        fix::g_core = was;
        return owed;
    }

    struct ReschedOwed : public ::testing::Test
    {
        void SetUp() override { fix::reset(); }
        void TearDown() override { fix::reset(); }
    };

    TEST_F(ReschedOwed, an_owed_reschedule_survives_a_consumer_that_does_not_reschedule)
    {
        // No dispatch runs here, so the cell stays published and the raise it preceded never
        // reaches the scheduler.
        owe_as(CORE_A, 1u << CORE_B);
        EXPECT_EQ(fix::raise_total(), 0u)
            << "publishing an owed reschedule must not raise anything by itself";

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 1u)
            << "core B left kernel state with a reschedule owed to it and restored no raise: "
               "the intent died with the raise that was absorbed, which is the starvation this "
               "cell exists to prevent";
    }

    TEST_F(ReschedOwed, the_release_reads_the_cell_and_does_not_clear_it)
    {
        owe_as(CORE_A, 1u << CORE_B);
        bracket_as(CORE_B);

        ASSERT_EQ(fix::g_raised[CORE_B], 1u) << "the arm needs the raise to have been restored";
        EXPECT_NE(owed_as(CORE_B), 0)
            << "the release consumed the cell: the raise it restored can be absorbed again by "
               "the very poll that absorbed the first one, and then nothing is left to say a "
               "reschedule was ever owed";
        EXPECT_NE(dispatch_as(CORE_B), 0)
            << "the dispatch that finally enters the scheduler must still find the ask standing";
    }

    TEST_F(ReschedOwed, only_the_take_clears_the_cell)
    {
        owe_as(CORE_A, 1u << CORE_B);

        bracket_as(CORE_B);
        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 2u)
            << "every release before the scheduler runs must re-arm the raise, since each one "
               "may be the last chance this core takes before going idle";

        ASSERT_NE(dispatch_as(CORE_B), 0) << "the ask stood until the dispatch took it";

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 2u)
            << "the ask was consumed by the dispatch, so a later release owes no raise and the "
               "core is not left raising a doorbell at itself forever";
    }

    TEST_F(ReschedOwed, the_raise_waits_for_the_outermost_release)
    {
        owe_as(CORE_A, 1u << CORE_B);

        fix::g_core = CORE_B;
        kickos::klock_enter();
        kickos::klock_enter();
        kickos::klock_leave();
        EXPECT_EQ(fix::g_raised[CORE_B], 0u)
            << "an inner release restored the raise: this core has not left kernel state and "
               "the raise would be absorbed again by its own outer section";

        kickos::klock_leave();
        EXPECT_EQ(fix::g_raised[CORE_B], 1u)
            << "the outermost release is where this core leaves kernel state and is where the "
               "owed raise has to be restored";
    }

    TEST_F(ReschedOwed, the_acquisition_does_not_restore_the_raise)
    {
        owe_as(CORE_A, 1u << CORE_B);

        fix::g_core = CORE_B;
        kickos::klock_enter();
        EXPECT_EQ(fix::g_raised[CORE_B], 0u)
            << "the acquisition restored the raise: klock_enter is opening a section whose "
               "caller has work to do inside it, and a raise taken there re-enters this core "
               "before that work has run";
        kickos::klock_leave();
    }

    TEST_F(ReschedOwed, the_cross_core_word_is_released_before_the_raise_is_restored)
    {
        owe_as(CORE_A, 1u << CORE_B);
        bracket_as(CORE_B);

        ASSERT_EQ(fix::g_raised[CORE_B], 1u) << "the arm needs the raise to have been restored";
        EXPECT_EQ(fix::g_held_at_raise, 0u)
            << "the raise was restored with the cross-core word still held: the dispatch it "
               "wakes takes that word, and this core would be holding what it wakes itself to "
               "wait for";
    }

    TEST_F(ReschedOwed, a_release_elsewhere_does_not_restore_a_peers_raise)
    {
        owe_as(CORE_A, 1u << CORE_B);

        bracket_as(CORE_A);
        EXPECT_EQ(fix::raise_total(), 0u)
            << "core A's own release answered a reschedule owed to core B: the cell is keyed by "
               "target and one core may not answer another's";

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 1u) << "core B's release still owes the raise";
    }

    TEST_F(ReschedOwed, an_ask_arriving_while_the_raise_is_restored_is_not_swallowed)
    {
        owe_as(CORE_A, 1u << CORE_B);
        // The peer asks again at the instant the release is restoring the raise.
        fix::g_raise_action = []() { owe_as(CORE_A, 1u << CORE_B); };

        bracket_as(CORE_B);
        ASSERT_EQ(fix::g_raised[CORE_B], 1u) << "the arm needs one raise restored";

        ASSERT_NE(dispatch_as(CORE_B), 0) << "the dispatch takes what stands";
        EXPECT_EQ(owed_as(CORE_B), 0)
            << "the take stores the sequence it read, so it answers exactly what it saw";

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 1u)
            << "the ask made during the restore was taken by the dispatch above and owes no "
               "further raise";
    }

    TEST_F(ReschedOwed, every_peers_ask_is_consumed_by_one_take)
    {
        owe_as(CORE_A, 1u << CORE_B);
        owe_as(CORE_B, 1u << CORE_B);

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 1u)
            << "two asks standing at once are one raise, not two";

        ASSERT_NE(dispatch_as(CORE_B), 0) << "the asks stood until the dispatch took them";
        EXPECT_EQ(owed_as(CORE_B), 0)
            << "the take consumed every row, not just the first that differed";
    }

    TEST_F(ReschedOwed, take_answers_once_per_ask)
    {
        EXPECT_EQ(dispatch_as(CORE_B), 0) << "nothing has been asked, so nothing stands";

        owe_as(CORE_A, 1u << CORE_B);
        EXPECT_NE(dispatch_as(CORE_B), 0) << "the ask stands and must be reported";
        EXPECT_EQ(dispatch_as(CORE_B), 0)
            << "the ask was consumed by the take above and may not stand a second time";
    }

    TEST_F(ReschedOwed, owed_does_not_consume)
    {
        owe_as(CORE_A, 1u << CORE_B);
        EXPECT_NE(owed_as(CORE_B), 0) << "the ask stands";
        EXPECT_NE(owed_as(CORE_B), 0) << "reading the cell must not clear it";
        EXPECT_NE(owed_as(CORE_B), 0) << "and must not clear it on a third read either";
        EXPECT_NE(dispatch_as(CORE_B), 0) << "the take still finds it";
    }

    TEST_F(ReschedOwed, an_ask_naming_no_core_owes_nothing)
    {
        owe_as(CORE_A, 0);
        bracket_as(CORE_B);
        bracket_as(CORE_A);
        EXPECT_EQ(fix::raise_total(), 0u)
            << "an empty mask named nobody and must cost no raise anywhere";
        EXPECT_EQ(owed_as(CORE_A), 0) << "nor may it owe the initiator itself";
        EXPECT_EQ(owed_as(CORE_B), 0) << "nor the core it did not name";
    }

    TEST_F(ReschedOwed, the_ask_publishes_the_cell_before_it_raises_the_doorbell)
    {
        ask_as(CORE_A, 1u << CORE_B);

        ASSERT_EQ(fix::g_sends, 1u) << "the ask must go over exactly one cross-core raise";
        EXPECT_EQ(fix::g_owed_at_send[CORE_B], 1u)
            << "the doorbell was raised at core B before the ask against B stood. The raise is "
               "an edge, and the acquire loop's poll acknowledges it without entering any "
               "scheduler: a poll landing in that window absorbs the raise while the cell still "
               "reads clean, and nothing is left to say a reschedule was ever owed";
    }

    TEST_F(ReschedOwed, the_ask_is_what_a_raise_absorbed_by_a_poll_survives_on)
    {
        ask_as(CORE_A, 1u << CORE_B);
        // The seam raised nothing at core B, which is the poll having absorbed the edge: the
        // consumer acknowledged the raise and entered no scheduler.
        ASSERT_EQ(fix::g_raised[CORE_B], 0u) << "no release has run on core B yet";

        bracket_as(CORE_B);
        EXPECT_EQ(fix::g_raised[CORE_B], 1u)
            << "core B left kernel state with the scheduler's ask standing against it and "
               "restored no raise: the ask died with the edge a poll absorbed, which is the "
               "starvation this cell exists to prevent";
        EXPECT_NE(dispatch_as(CORE_B), 0)
            << "and the dispatch the restored raise wakes must still find the ask standing";
    }

    TEST_F(ReschedOwed, the_ask_raises_every_core_it_names)
    {
        ask_as(CORE_A, (1u << CORE_A) | (1u << CORE_B));

        EXPECT_EQ(fix::g_sent_mask, (1u << CORE_A) | (1u << CORE_B))
            << "the raise must name the whole mask asked: the backend services the calling "
               "core's own bit itself rather than raising it, and a mask short of that bit "
               "leaves the caller's own request cell unbumped";
        EXPECT_NE(owed_as(CORE_B), 0) << "the peer it named is owed a reschedule";
        EXPECT_EQ(owed_as(CORE_A), 0)
            << "the asking core owes itself nothing: it is already in the kernel and reaches "
               "its own scheduler on the way out, where a cell against itself would instead "
               "have every release raise a doorbell at this core until a dispatch consumed it";
    }

    TEST_F(ReschedOwed, an_ask_naming_nobody_owes_nobody)
    {
        ask_as(CORE_A, 0);

        EXPECT_EQ(owed_as(CORE_A), 0) << "an empty mask may not owe the initiator";
        EXPECT_EQ(owed_as(CORE_B), 0) << "nor the core it did not name";
        bracket_as(CORE_A);
        bracket_as(CORE_B);
        EXPECT_EQ(fix::raise_total(), 0u)
            << "and no release anywhere owes a raise on the strength of it";
    }
}
