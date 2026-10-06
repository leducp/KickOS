// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/include/kickos/presync.h.

#include <kickos/presync.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    constexpr size_t G = 4096;
    constexpr arch_phys_addr_t PA = 0x40000000u;
}

TEST(Presync, add_refuses_an_empty_span_a_full_record_and_a_span_too_wide_to_hold)
{
    kickos::PresyncSpans r;
    EXPECT_FALSE(kickos::presync_add(r, PA, 0));
    EXPECT_FALSE(kickos::presync_add(r, PA, static_cast<size_t>(UINT32_MAX) + 1u));
    for (size_t i = 0; i < kickos::PRESYNC_SPANS; i++)
    {
        EXPECT_TRUE(kickos::presync_add(r, PA + i * 16u * G, 1));
    }
    EXPECT_FALSE(kickos::presync_add(r, PA + 0x1000000u, 1));
    EXPECT_EQ(r.count, kickos::PRESYNC_SPANS);
}

TEST(Presync, covers_only_a_span_inside_one_noted_span)
{
    kickos::PresyncSpans r;
    ASSERT_TRUE(kickos::presync_add(r, PA, 4));
    ASSERT_TRUE(kickos::presync_add(r, PA + 4u * G, 4));
    EXPECT_TRUE(kickos::presync_covers(r, PA, 4, G));
    EXPECT_TRUE(kickos::presync_covers(r, PA + G, 2, G));
    EXPECT_TRUE(kickos::presync_covers(r, PA + 4u * G, 4, G));
    // Adjacent noted spans are two spans: a mapping across their seam is not excused.
    EXPECT_FALSE(kickos::presync_covers(r, PA + 2u * G, 4, G));
    EXPECT_FALSE(kickos::presync_covers(r, PA, 5, G));
    EXPECT_FALSE(kickos::presync_covers(r, PA - G, 1, G));
    kickos::PresyncSpans const none;
    EXPECT_FALSE(kickos::presync_covers(none, PA, 1, G));
}

TEST(Presync, meets_any_shared_granule_and_no_neighbour)
{
    kickos::PresyncSpans r;
    ASSERT_TRUE(kickos::presync_add(r, PA + 4u * G, 4));
    EXPECT_TRUE(kickos::presync_meets(r, PA + 7u * G, 1, G));
    EXPECT_TRUE(kickos::presync_meets(r, PA, 5, G));
    EXPECT_TRUE(kickos::presync_meets(r, PA, 64, G));
    EXPECT_FALSE(kickos::presync_meets(r, PA, 4, G));
    EXPECT_FALSE(kickos::presync_meets(r, PA + 8u * G, 4, G));
}

TEST(Presync, pool_pages_counts_the_granules_inside_the_pool_alone)
{
    arch_phys_addr_t const lo = PA + 4u * G;
    arch_phys_addr_t const hi = PA + 8u * G;
    EXPECT_EQ(kickos::presync_pool_pages(PA + 4u * G, 4, G, lo, hi), 4u);
    EXPECT_EQ(kickos::presync_pool_pages(PA, 6, G, lo, hi), 2u);
    EXPECT_EQ(kickos::presync_pool_pages(PA + 6u * G, 6, G, lo, hi), 2u);
    EXPECT_EQ(kickos::presync_pool_pages(PA, 16, G, lo, hi), 4u);
    // A window over memory no pool owns, the AMP user share's, owes nothing.
    EXPECT_EQ(kickos::presync_pool_pages(PA + 8u * G, 512, G, lo, hi), 0u);
    EXPECT_EQ(kickos::presync_pool_pages(PA, 4, G, lo, hi), 0u);
    EXPECT_EQ(kickos::presync_pool_pages(PA, 4, 0, lo, hi), 0u);
}

TEST(Presync, only_a_live_record_covering_the_span_excuses_and_a_refusal_marks_it)
{
    kickos::PresyncRecord r;
    ASSERT_TRUE(kickos::presync_add(r.noted, PA, 4));
    EXPECT_FALSE(kickos::presync_excuses(r, PA, 4, G)) << "noted but not live";
    EXPECT_TRUE(r.refused);
    r.refused = false;
    r.live = true;
    EXPECT_TRUE(kickos::presync_excuses(r, PA + G, 2, G));
    EXPECT_FALSE(r.refused);
    EXPECT_FALSE(kickos::presync_excuses(r, PA + 2u * G, 4, G));
    EXPECT_TRUE(r.refused);
}

TEST(Presync, a_completed_edit_drops_a_live_record_it_meets_alone)
{
    kickos::PresyncRecord r;
    ASSERT_TRUE(kickos::presync_add(r.noted, PA, 4));
    EXPECT_FALSE(kickos::presync_dropped_by(r, PA, 4, G)) << "a dead record has nothing to lose";
    r.live = true;
    EXPECT_TRUE(kickos::presync_dropped_by(r, PA + 3u * G, 1, G));
    EXPECT_FALSE(kickos::presync_dropped_by(r, PA + 4u * G, 1, G));
    // A span this record's own call edited is not a reason to drop it.
    ASSERT_TRUE(kickos::presync_add(r.edits, PA + 8u * G, 1));
    EXPECT_FALSE(kickos::presync_dropped_by(r, PA + 8u * G, 1, G));
}
