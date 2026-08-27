// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/mem/vrange.cc. The list is validation data over a granule: no
// hardware, no page tables and no kernel state, so it is gated here rather than waiting for
// a board, and it is wired to a domain a step later.
//
// The granule is passed EXPLICITLY by every arm, never taken from a constant, because F7's
// freeze is that the figure is the arch's answer.

#include <kickos/vrange.h>
#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    constexpr size_t G = 4096;
    constexpr uintptr_t BASE = 0x2000000;

    kickos::VirtualRanges made(size_t granule = G)
    {
        kickos::VirtualRanges v;
        EXPECT_TRUE(v.init(granule));
        return v;
    }
}

TEST(VRange, init_refuses_a_granule_that_is_not_a_power_of_two)
{
    kickos::VirtualRanges v;
    EXPECT_FALSE(v.init(0));
    EXPECT_FALSE(v.init(3000));
    // Refused leaves the list answering nothing rather than answering at some other granule.
    EXPECT_FALSE(v.reserve(BASE, 1));
    EXPECT_FALSE(v.covers(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.init(8192));
    EXPECT_TRUE(v.reserve(BASE, 1));
}

TEST(VRange, reserve_refuses_a_base_below_the_granule)
{
    kickos::VirtualRanges v = made();
    EXPECT_FALSE(v.reserve(BASE + 1, 1));
    EXPECT_FALSE(v.reserve(BASE, 0));
    EXPECT_TRUE(v.reserve(BASE, 1));
}

TEST(VRange, reserve_refuses_an_overlap_from_either_side)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 4));
    EXPECT_FALSE(v.reserve(BASE, 1));           // same base
    EXPECT_FALSE(v.reserve(BASE + 3 * G, 4));   // straddles the top
    EXPECT_FALSE(v.reserve(BASE - G, 2));       // straddles the bottom
    EXPECT_TRUE(v.reserve(BASE + 4 * G, 1));    // flush above, no overlap
    EXPECT_TRUE(v.reserve(BASE - G, 1));        // flush below, no overlap
    EXPECT_EQ(v.count(), 3u);
}

TEST(VRange, reserve_refuses_a_range_that_wraps)
{
    kickos::VirtualRanges v = made();
    uintptr_t const top = ~static_cast<uintptr_t>(0) - (G - 1);
    EXPECT_FALSE(v.reserve(top, 2));
    EXPECT_EQ(v.count(), 0u);
}

TEST(VRange, a_reservation_admits_no_pointer_until_it_is_granted)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 2));
    // F10: allocation reserves and maps nothing, so the entry path must still refuse it.
    EXPECT_FALSE(v.covers(BASE, 8, ARCH_MAP_R));
    ASSERT_TRUE(v.grant(BASE, 2, ARCH_MAP_R | ARCH_MAP_W));
    EXPECT_TRUE(v.covers(BASE, 8, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, 2 * G, ARCH_MAP_R | ARCH_MAP_W));
}

TEST(VRange, grant_names_a_reservation_this_list_made)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 2));
    EXPECT_FALSE(v.grant(BASE + G, 1, ARCH_MAP_R));  // inside it, but not its base
    EXPECT_FALSE(v.grant(BASE, 1, ARCH_MAP_R));      // its base, but a different extent
    EXPECT_FALSE(v.grant(BASE, 2, 0));               // no right at all
    EXPECT_TRUE(v.grant(BASE, 2, ARCH_MAP_R));
}

TEST(VRange, covers_refuses_a_right_the_range_does_not_carry)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, 4, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, 4, ARCH_MAP_W));
    EXPECT_FALSE(v.covers(BASE, 4, ARCH_MAP_R | ARCH_MAP_W));
}

TEST(VRange, covers_refuses_a_range_that_leaves_the_entry)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, G, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, G + 1, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE - 1, 2, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, 0, ARCH_MAP_R));
}

TEST(VRange, covers_refuses_a_range_spanning_two_adjacent_granted_entries)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    ASSERT_TRUE(v.reserve(BASE + G, 1));
    ASSERT_TRUE(v.grant(BASE + G, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, G, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE + G, G, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE + G - 1, 2, ARCH_MAP_R));
}

TEST(VRange, covers_refuses_a_length_that_wraps)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, ~static_cast<size_t>(0), ARCH_MAP_R));
}

TEST(VRange, release_frees_the_slot_and_the_address_space)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_FALSE(v.release(BASE + G));
    EXPECT_TRUE(v.release(BASE));
    EXPECT_FALSE(v.release(BASE));
    EXPECT_EQ(v.count(), 0u);
    EXPECT_FALSE(v.covers(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.reserve(BASE, 1));
}

TEST(VRange, the_slot_bound_refuses_rather_than_evicting)
{
    kickos::VirtualRanges v = made();
    size_t const cap = kickos::VirtualRanges::capacity();
    for (size_t i = 0; i < cap; i++)
    {
        ASSERT_TRUE(v.reserve(BASE + i * 2 * G, 1)) << "slot " << i;
    }
    EXPECT_EQ(v.count(), cap);
    EXPECT_FALSE(v.reserve(BASE + cap * 2 * G, 1));
    // The refusal costs nothing that was already there.
    EXPECT_EQ(v.count(), cap);
    ASSERT_TRUE(v.release(BASE));
    EXPECT_TRUE(v.reserve(BASE + cap * 2 * G, 1));
}

TEST(VRange, at_walks_slots_and_reports_what_was_stored)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 3));
    ASSERT_TRUE(v.grant(BASE, 3, ARCH_MAP_R | ARCH_MAP_X));
    size_t seen = 0;
    for (size_t i = 0; i < kickos::VirtualRanges::capacity(); i++)
    {
        kickos::VirtualRange const* const r = v.at(i);
        if (r == nullptr)
        {
            continue;
        }
        seen++;
        EXPECT_EQ(r->base, BASE);
        EXPECT_EQ(r->pages, 3u);
        EXPECT_EQ(r->rights, static_cast<uint32_t>(ARCH_MAP_R | ARCH_MAP_X));
        EXPECT_EQ(r->state, kickos::VirtualState::Granted);
    }
    EXPECT_EQ(seen, 1u);
    EXPECT_EQ(v.at(kickos::VirtualRanges::capacity()), nullptr);
}

TEST(VRange, the_granule_is_the_arch_answer_and_not_a_constant)
{
    // A list at a 64 KiB granule must count in 64 KiB, or a caller that asked for one page
    // would be admitted over sixteen.
    kickos::VirtualRanges v = made(65536);
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, 65536, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, 65537, ARCH_MAP_R));
    EXPECT_FALSE(v.reserve(BASE + 4096, 1)); // sub-granule base
}
