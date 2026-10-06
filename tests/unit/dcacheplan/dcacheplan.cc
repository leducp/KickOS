// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A maintenance call runs by line below the cache's size, once over every set and way from it
// up, not at all over an empty range, and refuses a wrapping one.

#include "arch/arm/armv7m/dcache_plan.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <vector>

namespace
{
    using kickos::armv7m::DcachePlan;
    using kickos::armv7m::dcache_plan;

    // The Cortex-M7's 32 KiB data cache: 32-byte lines, 4 ways, 256 sets (TRM Table 3-7, p.3-11).
    constexpr uint32_t M7_32K = 0xF01FE019u;
    constexpr size_t SIZE = 32u * 1024u;
    constexpr uintptr_t TOP_LINE = UINTPTR_MAX & ~static_cast<uintptr_t>(31u);

    // The lines a plan visits, stopping past `cap` so a walk over the address space ends.
    std::vector<uintptr_t> walked(DcachePlan const& p, size_t cap)
    {
        std::vector<uintptr_t> seen;
        kickos::armv7m::dcache_each_line(p, [&seen, cap](uintptr_t a) {
            if (seen.size() <= cap)
            {
                seen.push_back(a);
            }
        });
        return seen;
    }
}

TEST(DcachePlan, BelowTheCacheSizeRunsByLine)
{
    DcachePlan const p = dcache_plan(M7_32K, 0x20000010u, SIZE - 32u);
    EXPECT_EQ(p.kind, DcachePlan::Kind::LINES);
    EXPECT_EQ(p.first, 0x20000000u);
    EXPECT_EQ(p.lines, SIZE / 32u);
    EXPECT_EQ(p.line, 32u);
}

TEST(DcachePlan, ARangeEndingOnALineTakesNoLineBeyond)
{
    std::vector<uintptr_t> const seen = walked(dcache_plan(M7_32K, 0x20000000u, 64u), 4u);
    EXPECT_EQ(seen, (std::vector<uintptr_t>{0x20000000u, 0x20000020u}));
}

TEST(DcachePlan, TheLastLineOfTheAddressSpaceIsOneLine)
{
    DcachePlan const p = dcache_plan(M7_32K, TOP_LINE + 4u, 8u);
    EXPECT_EQ(p.kind, DcachePlan::Kind::LINES);
    EXPECT_EQ(walked(p, 4u), (std::vector<uintptr_t>{TOP_LINE}));
}

TEST(DcachePlan, ARangeEndingAtTheTopOfTheAddressSpaceRunsByLine)
{
    DcachePlan const p = dcache_plan(M7_32K, TOP_LINE - 32u, 64u);
    EXPECT_EQ(p.kind, DcachePlan::Kind::LINES);
    EXPECT_EQ(walked(p, 4u), (std::vector<uintptr_t>{TOP_LINE - 32u, TOP_LINE}));
}

TEST(DcachePlan, TheCacheSizeAndAboveRunsBySetWay)
{
    EXPECT_EQ(dcache_plan(M7_32K, 0x20000000u, SIZE).kind, DcachePlan::Kind::SET_WAY);
    EXPECT_EQ(dcache_plan(M7_32K, 0x20000000u, 16u * SIZE).kind, DcachePlan::Kind::SET_WAY);
}

TEST(DcachePlan, AWrappingRangeIsRefused)
{
    EXPECT_EQ(dcache_plan(M7_32K, UINTPTR_MAX - 15u, 64u).kind, DcachePlan::Kind::WRAPS);
    EXPECT_EQ(dcache_plan(M7_32K, UINTPTR_MAX - 15u, 17u).kind, DcachePlan::Kind::WRAPS);
}

TEST(DcachePlan, AnEmptyRangeRunsNothing)
{
    EXPECT_EQ(dcache_plan(M7_32K, 0x20000000u, 0u).kind, DcachePlan::Kind::NONE);
}
