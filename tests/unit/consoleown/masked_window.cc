// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_kconsole_write needs no capability, so an unprivileged thread reaches
// console_tx_insert_line with a line of its own choosing. These cases pin the cost of that call
// in the unit that matters for interrupt latency: bytes pushed to the TX data register inside ONE
// masked span, each of which is a byte time at the line rate.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>

#include "console_seam.h"

namespace
{
    constexpr uint32_t kRing = 512u;
    constexpr uint32_t kCapacity = kRing - 1u;

    std::string pattern(size_t n, char first)
    {
        std::string s;
        s.reserve(n);
        for (size_t i = 0; i < n; i++)
        {
            s.push_back(static_cast<char>(first + static_cast<char>(i % 26u)));
        }
        return s;
    }

    class ConsoleTxMaskedWindow : public ::testing::Test
    {
    protected:
        void SetUp() override { consoleseam::reset(kRing); }
    };
}

// A line is one copy under the mask, and the only push inside it is the idle channel's prime.
TEST_F(ConsoleTxMaskedWindow, ALineThatFitsPushesAtMostThePrimeUnderTheMask)
{
    std::string const in = pattern(kCapacity, 'a');
    EXPECT_EQ(console_tx_insert_line(in.data(), in.size(), 0), static_cast<int>(in.size()));
    EXPECT_EQ(consoleseam::wire(), in);
    EXPECT_LE(consoleseam::max_masked_pushes(), 1u);
}

// 4096 bytes pushed with the mask held would be ~356 ms of interrupt-off time at 115200 8N1. A
// line wider than the ring is refused whole instead, and nothing reaches the device.
TEST_F(ConsoleTxMaskedWindow, ALineWiderThanTheRingIsRefusedWithoutAPush)
{
    std::string const in = pattern(4096, 'a');
    EXPECT_EQ(console_tx_insert_line(in.data(), in.size(), 0), 0);
    EXPECT_TRUE(consoleseam::wire().empty());
    EXPECT_EQ(consoleseam::max_masked_pushes(), 0u);
}

// With no TX interrupt the producer drains its own line, one byte per masked span, whatever the
// line's length.
TEST_F(ConsoleTxMaskedWindow, AProducerDrainPushesOneByteUnderTheMaskForEveryLineSize)
{
    for (size_t n = 1; n <= kCapacity; n *= 3u)
    {
        consoleseam::reset(kRing, -1);
        std::string const in = pattern(n, 'a');
        EXPECT_EQ(console_tx_insert_line(in.data(), in.size(), 0), static_cast<int>(n)) << "n=" << n;
        EXPECT_EQ(consoleseam::wire(), in) << "n=" << n;
        EXPECT_LE(consoleseam::max_masked_pushes(), 1u) << "n=" << n;
    }
}

// The console syscall adds up this count and answers -KOS_EAGAIN where it is 0, so a line
// is counted whole when it fits and not at all when it does not.
TEST_F(ConsoleTxMaskedWindow, InsertLineCountsWhatItQueuesAndNothingItRefuses)
{
    consoleseam::set_isr_runs_in_gap(false);
    consoleseam::set_slot_free(0);
    std::string const first = pattern(300, 'a');
    std::string const second = pattern(300, 'A');
    EXPECT_EQ(console_tx_insert_line(first.data(), first.size(), 0), 300);
    EXPECT_EQ(console_tx_insert_line(second.data(), second.size(), 0), 0);
    EXPECT_EQ(console_tx_insert_line(second.data(), 0, 0), 0);
    EXPECT_TRUE(consoleseam::wire().empty());
}
