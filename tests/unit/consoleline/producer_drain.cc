// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The drain a producer runs in its own context on a backend with no TX interrupt. It samples
// tail, pushes with interrupts OPEN, and commits the advance only against the tail that pass
// sampled. The open push is a preemption point: another thread's console_tx_flush_sync or a
// console_tx_deinit lands there and moves tail itself, and a blind advance then walks past
// head and transmits the ring's stale contents.
//
// EACH ARM SEATS ITS OWN INTERLEAVING in that push and reads the wire afterwards, so the
// posture is the same on every run. A seated call runs on the producer's own stack, which is
// where a preempting thread's work would be observed from.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>

#include "line_seam.h"

namespace
{
    constexpr uint32_t kRing = 64u;
    constexpr uint32_t kTightRing = 16u;
    constexpr int kIrqLine = 3;
    constexpr int kNoIrqLine = -1;
    char const kLine[] = "abcdefgh";
    constexpr size_t kLineLen = 8u;

    // The second push is the drain's first: the first is the insert's own prime.
    constexpr uint32_t kDrainsFirstPush = 2u;

    // The drain's LAST push: the prime took byte 1, so the loop pushes 2 through 8 and the
    // eighth is where taking the byte leaves tail == head. That is the only push during which
    // the ring reads EMPTY with a byte still going to the device.
    constexpr uint32_t kDrainsLastPush = 8u;

    void flush_from_inside_the_push(void)
    {
        console_tx_flush_sync();
    }

    void deinit_from_inside_the_push(void)
    {
        console_tx_deinit();
    }

    int g_note_took = -1;

    void insert_from_inside_the_push(void)
    {
        g_note_took = consoleline::write_line("ZZ", 2, 0);
    }

    // The drain TAKES its byte under the lock before the device write, so a mover of tail
    // landing in the open push cannot send it too: one byte, one sender, and the wire carries
    // the line once.
    std::string const kWireWithNoByteSentTwice = "abcdefgh";

    class ConsoleTxProducerDrain : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            consoleline::reset(kRing, kNoIrqLine);
            g_note_took = -1;
        }

        void expect_the_seat_ran_and_moved_tail()
        {
            EXPECT_TRUE(consoleline::push_seat_fired())
                << "the drain never reached the push the interleaving is seated in";
            EXPECT_GT(consoleline::nested_pushes(), 0u)
                << "the seated call drained nothing, so tail never moved inside the window "
                   "and this arm read an ordinary drain";
        }
    };
}

// A flush landing in the open push drains what the ring still holds. The byte already taken
// is not among them, so it goes out once and the flush carries the rest.
TEST_F(ConsoleTxProducerDrain, AFlushInTheDrainsPushDoesNotWalkTailPastHead)
{
    consoleline::run_in_push(kDrainsFirstPush, flush_from_inside_the_push);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    expect_the_seat_ran_and_moved_tail();
    EXPECT_EQ(consoleline::wire(), kWireWithNoByteSentTwice);
}

// The same window, with console_tx_deinit as the mover of tail: it flushes and then disarms,
// and the drain loop has no arming check of its own.
TEST_F(ConsoleTxProducerDrain, ADeinitInTheDrainsPushDoesNotWalkTailPastHead)
{
    consoleline::run_in_push(kDrainsFirstPush, deinit_from_inside_the_push);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    expect_the_seat_ran_and_moved_tail();
    EXPECT_EQ(console_tx_armed(), 0);
    EXPECT_EQ(consoleline::wire(), kWireWithNoByteSentTwice);
}

// THE RING READS EMPTY WHILE A TAKEN BYTE IS STILL GOING TO THE DEVICE. The drain takes its
// byte under the lock and pushes it with the lock open, so an insert landing between the two
// sees used() == 0. Priming on that reading would push its own first byte straight at the
// device while the drain's byte is in flight: two writers, and the line already on the wire is
// split. The insert primes only when no drain owns a byte.
TEST_F(ConsoleTxProducerDrain, AnInsertDoesNotPrimeWhileADrainOwnsAByteInFlight)
{
    consoleline::run_in_push(kDrainsLastPush, insert_from_inside_the_push);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    // Not the shared control: a nested PUSH is what this arm forbids, so it proves the window
    // was real by the seat firing and the seated line being taken instead.
    ASSERT_TRUE(consoleline::push_seat_fired())
        << "the drain never reached the push the insert is seated in";
    ASSERT_EQ(g_note_took, 1) << "the seated line was refused, so this arm read a full ring "
                                 "rather than the priming window";
    EXPECT_EQ(consoleline::max_push_depth(), 1u)
        << "a second writer pushed while the drain's byte was in flight";
    EXPECT_EQ(consoleline::wire(), std::string("abcdefghZZ"));
}

// SUSTAINED HEALTHY PRESSURE, which every other arm in this file stages a fault into. A small
// ring is filled and drained repeatedly across several line lengths: the wire has to carry
// every byte in order, no line may be refused while the ring is being emptied each time, the
// drain must not recurse, and the single-drainer flag must be clear at the end of each line
// rather than left standing by a path that returned early.
TEST_F(ConsoleTxProducerDrain, SustainedPressureLosesNoLineAndLeavesNoDrainStanding)
{
    std::string expected;
    for (uint32_t round = 0; round < 24u; round++)
    {
        size_t const len = 1u + (round % 11u);
        std::string line;
        for (size_t i = 0; i < len; i++)
        {
            line.push_back(static_cast<char>('a' + (i % 26u)));
        }
        ASSERT_EQ(consoleline::write_line(line.data(), line.size(), 0), 1)
            << "round " << round << " was refused, but the drain empties the ring each time";
        expected += line;
        ASSERT_EQ(consoleline::wire(), expected) << "round " << round << " left bytes behind";
    }
    EXPECT_EQ(consoleline::max_push_depth(), 1u) << "the drain recursed under pressure";
}

// A line inserted while a drain is running is queued and left to that drain, which re-reads
// head every pass and carries it out in order.
TEST_F(ConsoleTxProducerDrain, ALineInsertedInsideADrainIsCarriedByTheRunningDrain)
{
    consoleline::run_in_push(kDrainsFirstPush, insert_from_inside_the_push);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    ASSERT_TRUE(consoleline::push_seat_fired())
        << "the drain never reached the push the line is seated in";
    EXPECT_EQ(g_note_took, 1);
    EXPECT_EQ(consoleline::wire(), std::string("abcdefghZZ"));
}

// The same interleaving read at the mechanism: the seated line must not enter a second drain,
// which would put two movers of tail on one stack.
TEST_F(ConsoleTxProducerDrain, ALineInsertedInsideADrainEntersNoSecondDrain)
{
    consoleline::run_in_push(kDrainsFirstPush, insert_from_inside_the_push);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    ASSERT_TRUE(consoleline::push_seat_fired());
    EXPECT_EQ(consoleline::max_push_depth(), 1u)
        << "a push ran inside another push, so a second drain was entered";
}

// Every poll on this path is bounded. A channel that never frees a slot costs the queued bytes
// and returns, leaving the ring empty for the next line.
TEST_F(ConsoleTxProducerDrain, AWedgedChannelDiscardsTheRingRatherThanSpin)
{
    consoleline::reset(kTightRing, kNoIrqLine);
    consoleline::set_slot_free(0);
    std::string const line(kTightRing - 1u, 'Q');

    ASSERT_EQ(console_tx_insert_line(line.data(), line.size(), 0), 1);
    EXPECT_TRUE(consoleline::wire().empty());

    consoleline::set_slot_free(1);
    EXPECT_EQ(console_tx_insert_line(line.data(), line.size(), 0), 1)
        << "a line the size of the whole ring was refused, so the wedged drain left the ring "
           "holding bytes nothing will ever carry";
}

// A backend whose TX-empty ISR owns tail is left to it: the producer queues and returns, and
// only the prime is on the wire until the drain runs.
TEST_F(ConsoleTxProducerDrain, AChannelWithATxInterruptIsLeftToItsDrainIsr)
{
    consoleline::reset(kRing, kIrqLine);

    ASSERT_EQ(consoleline::write_line(kLine, kLineLen, 0), 1);
    EXPECT_EQ(consoleline::wire(), std::string("a"));

    consoleline::pump_tx_isr();
    EXPECT_EQ(consoleline::wire(), std::string(kLine, kLineLen));
    EXPECT_GT(consoleline::isr_entries(), 0u);
}
