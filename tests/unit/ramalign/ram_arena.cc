// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The allocator under arch_ram_alloc keeps the alignment run-up a block skipped, and the next
// block that fits it takes it; a block that would run across the bank line starts at it. Its copy
// in the composition tool places seeded sequences alike.

#include "ram_arena.h"

#include <gtest/gtest.h>

#include "gap_table.h"

using kickos::RamArena;
using kickos::ram_arena_alloc;

namespace
{
    constexpr size_t TOTAL = 0x100000;
    alignas(8192) unsigned char g_buffer[TOTAL];

    uintptr_t base()
    {
        return reinterpret_cast<uintptr_t>(g_buffer);
    }

    // The offset of the block, or SIZE_MAX when refused; `line` is an offset, 0 for none.
    size_t take(RamArena& a, size_t size, size_t align, size_t line = 0)
    {
        uintptr_t at = 0;
        if (line != 0)
        {
            at = base() + line;
        }
        void* const p = ram_arena_alloc(a, base(), TOTAL, size, align, at);
        if (p == nullptr)
        {
            return SIZE_MAX;
        }
        return reinterpret_cast<uintptr_t>(p) - base();
    }

    TEST(RamArena, ALaterSmallBlockTakesTheRunUpAnEarlierBlockSkipped)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x1d0, 16), 0u);
        EXPECT_EQ(take(a, 0x200, 0x200), 0x200u);
        EXPECT_EQ(take(a, 0x1000, 0x1000), 0x1000u);
        EXPECT_EQ(a.used, 0x2000u);
        EXPECT_EQ(take(a, 0x40, 0x40), 0x400u);
        EXPECT_EQ(take(a, 0x40, 0x40), 0x440u);
        EXPECT_EQ(a.used, 0x2000u);
    }

    TEST(RamArena, OneRangeIsKeptTheLargestRunUpAndAnotherIsLost)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x10, 16), 0u);
        EXPECT_EQ(take(a, 0x100, 0x100), 0x100u);
        EXPECT_EQ(a.gap_off, 0x10u);
        EXPECT_EQ(a.gap_len, 0xf0u);
        // A larger run-up replaces it.
        EXPECT_EQ(take(a, 0x400, 0x400), 0x400u);
        EXPECT_EQ(a.gap_off, 0x200u);
        EXPECT_EQ(a.gap_len, 0x200u);
        // An equal one does not.
        EXPECT_EQ(take(a, 0x600, 0x400), 0x800u);
        EXPECT_EQ(take(a, 0x400, 0x400), 0x1000u);
        EXPECT_EQ(a.gap_off, 0x200u);
        EXPECT_EQ(a.gap_len, 0x200u);
        // A smaller one is lost: [0x1610, 0x1800) is never handed out.
        EXPECT_EQ(take(a, 0x210, 16), 0x1400u);
        EXPECT_EQ(take(a, 0x400, 0x200), 0x1800u);
        EXPECT_EQ(a.gap_off, 0x200u);
        EXPECT_EQ(take(a, 0x200, 16), 0x200u);
        EXPECT_EQ(a.gap_len, 0u);
        EXPECT_EQ(take(a, 0x10, 16), 0x1c00u);
    }

    TEST(RamArena, TheKeptRangeIsTakenOnTheBlocksOwnAlignmentToTheByte)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x30, 16), 0u);
        EXPECT_EQ(take(a, 0x80, 0x80), 0x80u);
        EXPECT_EQ(a.gap_off, 0x30u);
        EXPECT_EQ(a.gap_len, 0x50u);
        // The range is long enough, the granule aligned start is not: 0x40 + 0x50 > 0x80.
        EXPECT_EQ(take(a, 0x50, 0x20), 0x100u);
        EXPECT_EQ(a.used, 0x150u);
        EXPECT_EQ(a.gap_len, 0x50u);
        // The lower piece is the larger and stays.
        EXPECT_EQ(take(a, 0x40, 0x20), 0x40u);
        EXPECT_EQ(a.gap_off, 0x30u);
        EXPECT_EQ(a.gap_len, 0x10u);
        EXPECT_EQ(take(a, 0x10, 0x10), 0x30u);
        EXPECT_EQ(a.gap_len, 0u);
    }

    TEST(RamArena, OnATieTheUpperPieceStays)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x30, 16), 0u);
        EXPECT_EQ(take(a, 0x80, 0x80), 0x80u);
        EXPECT_EQ(take(a, 0x30, 0x20), 0x40u);
        EXPECT_EQ(a.gap_off, 0x70u);
        EXPECT_EQ(a.gap_len, 0x10u);
    }

    TEST(RamArena, ARefusalLeavesTheArenaAndItsKeptRangeAsTheyWere)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x10, 16), 0u);
        EXPECT_EQ(take(a, 0x100, 0x100), 0x100u);
        EXPECT_EQ(take(a, TOTAL - 0x100, 16), SIZE_MAX);
        EXPECT_EQ(a.used, 0x200u);
        EXPECT_EQ(a.gap_off, 0x10u);
        EXPECT_EQ(a.gap_len, 0xf0u);
        EXPECT_EQ(take(a, 0xf0, 16), 0x10u);
    }

    TEST(RamArena, ABlockEndingAtTheLineStaysBelowIt)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x1fc0, 32, 0x2000), 0u);
        EXPECT_EQ(take(a, 0x40, 32, 0x2000), 0x1fc0u);
        EXPECT_EQ(a.used, 0x2000u);
        EXPECT_EQ(a.gap_len, 0u);
    }

    TEST(RamArena, ABlockStartingAtTheLineStaysThere)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x2000, 32, 0x2000), 0u);
        EXPECT_EQ(take(a, 0x60, 32, 0x2000), 0x2000u);
        EXPECT_EQ(a.used, 0x2060u);
        EXPECT_EQ(a.gap_len, 0u);
    }

    TEST(RamArena, ABlockThatWouldRunAcrossTheLineStartsAtItAndKeepsTheGapBelow)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x1f00, 32, 0x2000), 0u);
        EXPECT_EQ(take(a, 0x120, 32, 0x2000), 0x2000u);
        EXPECT_EQ(a.used, 0x2120u);
        EXPECT_EQ(a.gap_off, 0x1f00u);
        EXPECT_EQ(a.gap_len, 0x100u);
        // The gap below the line is the kept range, and a block that fits it takes it.
        EXPECT_EQ(take(a, 0x100, 32, 0x2000), 0x1f00u);
        EXPECT_EQ(a.gap_len, 0u);
        EXPECT_EQ(a.used, 0x2120u);
    }

    TEST(RamArena, AHopPastTheArenaIsRefusedAndLeavesItAsItWas)
    {
        RamArena a;
        EXPECT_EQ(take(a, 0x1f00, 32, TOTAL - 0x100), 0u);
        EXPECT_EQ(take(a, TOTAL - 0x1f00 - 0x80, 32, TOTAL - 0x100), SIZE_MAX);
        EXPECT_EQ(a.used, 0x1f00u);
        EXPECT_EQ(a.gap_len, 0u);
    }

    TEST(RamArena, TheCompositionToolPlacesSeededSequencesAlike)
    {
        size_t lined = 0;
        for (GapSequence const& seq : GAP_SEQUENCES)
        {
            RamArena a;
            for (GapStep const& step : seq.steps)
            {
                ASSERT_EQ(take(a, step.size, step.align, seq.line), step.offset);
                if (seq.line != 0)
                {
                    EXPECT_FALSE(step.offset < seq.line and step.offset + step.size > seq.line);
                }
            }
            if (seq.line != 0)
            {
                lined++;
            }
        }
        EXPECT_NE(lined, 0u);
    }
}
