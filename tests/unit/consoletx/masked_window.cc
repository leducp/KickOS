// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_kconsole_write needs no capability, so an unprivileged thread reaches
// console_tx_write with a buffer of its own choosing. These cases pin the cost of that
// call in the only unit that matters for interrupt latency: bytes pushed to the TX data
// register inside ONE masked span, each of which is a byte time at the line rate.

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>

#include "tx_seam.h"

namespace
{
    constexpr uint32_t kRing = 512u; // 511 usable
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
        void SetUp() override { consoletxfix::reset(kRing); }
    };
}

// A burst that fits is one enqueue: the only masked push is the idle->busy prime.
TEST_F(ConsoleTxMaskedWindow, BurstThatFitsPushesAtMostThePrimeUnderTheMask)
{
    std::string const in = pattern(128, 'a');
    console_tx_write(in.data(), in.size());
    EXPECT_EQ(consoletxfix::wire(), in);
    EXPECT_LE(consoletxfix::max_masked_pushes(), 1u);
}

// 4096 bytes pushed with the mask held would be ~356 ms of interrupt-off time at 115200 8N1.
TEST_F(ConsoleTxMaskedWindow, OversizedBurstDoesNotBitBangUnderTheMask)
{
    std::string const in = pattern(4096, 'a');
    console_tx_write(in.data(), in.size());
    EXPECT_EQ(consoletxfix::wire(), in);
    EXPECT_LE(consoletxfix::max_masked_pushes(), 1u);
}

// Anti-vacuity for the case above: the bound must be met by OPENING the mask, not by
// pushing nothing. A write of eight ring-fulls cannot complete in fewer than seven gaps.
TEST_F(ConsoleTxMaskedWindow, OversizedBurstOpensTheMaskOncePerRingFull)
{
    std::string const in = pattern(8u * kCapacity, 'a');
    console_tx_write(in.data(), in.size());
    EXPECT_EQ(consoletxfix::wire().size(), in.size());
    EXPECT_GE(consoletxfix::gap_count(), 7u);
}

// No masked span may exceed the ring, whatever the burst: the enqueue copies at most
// space() bytes and the prime is one.
TEST_F(ConsoleTxMaskedWindow, MaskedSpanIsBoundedByTheRingForEveryBurstSize)
{
    for (size_t n = 1; n <= 4u * kCapacity; n *= 3u)
    {
        consoletxfix::reset(kRing);
        std::string const in = pattern(n, 'a');
        console_tx_write(in.data(), in.size());
        EXPECT_EQ(consoletxfix::wire(), in) << "n=" << n;
        EXPECT_LE(consoletxfix::max_masked_pushes(), kCapacity) << "n=" << n;
    }
}

// A drain ISR that cannot reach the CPU is the one case the synchronous fallback exists for,
// and it must terminate with the TX register permanently full.
TEST_F(ConsoleTxMaskedWindow, WedgedChannelWithNoDrainTerminates)
{
    consoletxfix::set_isr_runs_in_gap(false);
    consoletxfix::set_slot_free(0);
    std::string const in = pattern(4096, 'a');
    console_tx_write(in.data(), in.size());
    SUCCEED(); // reaching here is the assertion
}

// Releasing the mask between chunks bounds the window at the cost of per-call atomicity: a
// concurrent producer CAN land in the gap. What must survive is each stream's own order and
// every one of its bytes.
namespace
{
    void seat_second_producer(void)
    {
        static char const other[] = "ZZZZ";
        console_tx_write(other, 4);
    }
}

TEST_F(ConsoleTxMaskedWindow, ProducerSeatedInTheGapInterleavesWithoutLosingBytes)
{
    consoletxfix::run_in_first_gap(seat_second_producer);
    std::string const in = pattern(4u * kCapacity, 'a');
    console_tx_write(in.data(), in.size());

    std::string mine;
    size_t theirs = 0;
    for (char const c : consoletxfix::wire())
    {
        if (c == 'Z')
        {
            theirs++;
        }
        else
        {
            mine.push_back(c);
        }
    }
    EXPECT_EQ(mine, in);
    EXPECT_EQ(theirs, 4u);
}

// Chunking gives up more than atomicity against another producer: console_tx_deinit can
// land in a mask gap too. It detaches the drain handler and NVIC-masks the line, so a byte
// queued after it is drained by nothing, and console_tx_flush_sync returns without touching
// a disarmed ring, so not even the panic path recovers it.
namespace
{
    // Past one ring-full, but inside the next chunk, so the write completes in the loop and
    // never reaches the synchronous fallback that would empty the ring anyway.
    constexpr size_t kSpillPastOneRing = 300u;

    void seat_deinit(void)
    {
        console_tx_deinit();
        consoletxfix::set_isr_runs_in_gap(false); // irq_detach plus the NVIC mask
    }
}

TEST_F(ConsoleTxMaskedWindow, DeinitSeatedInTheGapLosesNoBytes)
{
    consoletxfix::run_in_first_gap(seat_deinit);
    std::string const in = pattern(kCapacity + kSpillPastOneRing, 'a');
    console_tx_write(in.data(), in.size());
    console_tx_flush_sync(); // refused on a disarmed ring: whatever is stranded stays stranded
    EXPECT_EQ(console_tx_armed(), 0);
    EXPECT_EQ(consoletxfix::wire(), in);
}

TEST_F(ConsoleTxMaskedWindow, DeinitSeatedInTheGapLeavesTheTxInterruptDisabled)
{
    consoletxfix::run_in_first_gap(seat_deinit);
    std::string const in = pattern(kCapacity + kSpillPastOneRing, 'a');
    console_tx_write(in.data(), in.size());
    EXPECT_EQ(console_tx_armed(), 0);
    EXPECT_FALSE(consoletxfix::tx_irq_enabled());
}

// A zero-length write has no chunk to queue, so the chunk loop never runs. Falling through
// it into the synchronous fallback drains every byte already queued with the mask held.
TEST_F(ConsoleTxMaskedWindow, ZeroLengthWritePushesNothingUnderTheMask)
{
    consoletxfix::set_isr_runs_in_gap(false);
    std::string const queued = pattern(400, 'a');
    console_tx_write(queued.data(), queued.size());
    ASSERT_EQ(consoletxfix::max_masked_pushes(), 1u); // the prime; 399 bytes sit in the ring
    ASSERT_EQ(consoletxfix::wire().size(), 1u);

    console_tx_write(queued.data(), 0);
    EXPECT_EQ(consoletxfix::max_masked_pushes(), 1u);
    EXPECT_EQ(consoletxfix::wire().size(), 1u);
}
