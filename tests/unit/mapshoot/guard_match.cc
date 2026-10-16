// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Whether an x86_64 core's CR4, as it reads back, holds the boot core's SMEP and SMAP.

#include <kickos/arch/ring3.h>

#include <gtest/gtest.h>

using kickos::x86_64::guard_matches;

namespace
{
    constexpr uint64_t SMEP = 1ull << 20;
    constexpr uint64_t SMAP = 1ull << 21;
    // PAE, OSFXSR, OSXMMEXCPT and OSXSAVE: bits every core here carries.
    constexpr uint64_t BASE = 0x00040620ull;
    constexpr uint64_t LA57 = 1ull << 12;
}

TEST(GuardMatch, BothOnBothCoresMatch)
{
    EXPECT_TRUE(guard_matches(BASE | SMEP | SMAP, BASE | SMEP | SMAP));
}

TEST(GuardMatch, NeitherOnBothCoresMatches)
{
    EXPECT_TRUE(guard_matches(BASE, BASE));
}

TEST(GuardMatch, SmepAloneOnBothCoresMatches)
{
    EXPECT_TRUE(guard_matches(BASE | SMEP, BASE | SMEP));
}

TEST(GuardMatch, AnApLackingSmapIsRefused)
{
    EXPECT_FALSE(guard_matches(BASE | SMEP | SMAP, BASE | SMEP));
}

TEST(GuardMatch, AnApWithSmapTheBootCoreLacksIsRefused)
{
    EXPECT_FALSE(guard_matches(BASE | SMEP, BASE | SMEP | SMAP));
}

TEST(GuardMatch, AnApLackingSmepIsRefused)
{
    EXPECT_FALSE(guard_matches(BASE | SMEP | SMAP, BASE | SMAP));
}

TEST(GuardMatch, OtherBitsDoNotDecide)
{
    EXPECT_TRUE(guard_matches(BASE | SMEP | SMAP, SMEP | SMAP | LA57));
}
