// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/mem/vrange.cc.
//
// The granule is passed explicitly by every arm, never taken from a constant, because F7's
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
    // At a 64 KiB granule a caller that asked for one page must not be admitted over
    // sixteen.
    kickos::VirtualRanges v = made(65536);
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R));
    EXPECT_TRUE(v.covers(BASE, 65536, ARCH_MAP_R));
    EXPECT_FALSE(v.covers(BASE, 65537, ARCH_MAP_R));
    EXPECT_FALSE(v.reserve(BASE + 4096, 1)); // sub-granule base
}

TEST(VRange, find_answers_the_one_entry_a_range_lies_inside)
{
    // An address this list never reserved is in no entry, a cross-task self-grant
    // included (F10).
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 4));
    EXPECT_EQ(v.find(BASE - 1, 1), nullptr);
    EXPECT_EQ(v.find(BASE + 4 * G, 1), nullptr);
    EXPECT_EQ(v.find(BASE, 0), nullptr);
    kickos::VirtualRange const* const e = v.find(BASE + 7, 9);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->base, BASE);   // the entry's own extent, not the caller's slice
    EXPECT_EQ(e->pages, 4u);
    // A reservation answers too, unlike covers(): the self-grant has to find the entry
    // that has not been granted yet.
    EXPECT_EQ(e->state, kickos::VirtualState::Reserved);
    EXPECT_FALSE(v.covers(BASE, 1, ARCH_MAP_R));
}

TEST(VRange, find_refuses_a_range_leaving_the_entry_or_wrapping)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 2));
    ASSERT_TRUE(v.reserve(BASE + 2 * G, 2)); // flush above, a second entry
    EXPECT_EQ(v.find(BASE + G, 3 * G), nullptr); // spans two entries: no single answer
    EXPECT_EQ(v.find(BASE, SIZE_MAX), nullptr);
}

TEST(VRange, the_flags_travel_with_the_entry_and_survive_the_grant)
{
    // Teardown reads them: a borrowed range is unmapped and freed by nobody, an image extent
    // is nobody's reservation, and both answers have to outlive the grant that maps it.
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1, kickos::VR_IMAGE | kickos::VR_BORROWED));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R | ARCH_MAP_X, ARCH_MAP_NORMAL));
    kickos::VirtualRange const* const e = v.find(BASE, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->flags, static_cast<uint8_t>(kickos::VR_IMAGE | kickos::VR_BORROWED));
    EXPECT_EQ(e->memtype, static_cast<uint8_t>(ARCH_MAP_NORMAL));
    // Released and re-reserved plain: a recycled slot must not carry the old provenance.
    ASSERT_TRUE(v.release(BASE));
    ASSERT_TRUE(v.reserve(BASE, 1));
    kickos::VirtualRange const* const f = v.find(BASE, 1);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->flags, 0u);
    EXPECT_EQ(f->memtype, 0u);
}

TEST(VRange, the_memory_type_is_recorded_by_the_grant_that_maps_it)
{
    // The already-mapped short-circuit asks whether the range carries these attributes, so
    // a second grant at another type has to be distinguishable from the first.
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NOCACHE));
    kickos::VirtualRange const* e = v.find(BASE, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->memtype, static_cast<uint8_t>(ARCH_MAP_NOCACHE));
    ASSERT_TRUE(v.grant(BASE, 1, ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL));
    e = v.find(BASE, 1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->memtype, static_cast<uint8_t>(ARCH_MAP_NORMAL));
}

TEST(VRange, overlaps_fails_closed_on_an_extent_whose_arithmetic_wraps)
{
    // overlaps is public and advertises no precondition, so it owns its own extent
    // arithmetic. Its only caller checks first today, which is what makes a wrapped end
    // unwitnessed: a computed end below the base makes the loop's comparisons meet nothing
    // and the range reads as free.
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 1));
    // The byte count itself wraps.
    EXPECT_TRUE(v.overlaps(BASE + 64 * G, (SIZE_MAX / G) + 2u));
    // The count is exact and the end wraps.
    uintptr_t const top = ~static_cast<uintptr_t>(0) - (2u * G) + 1u;
    EXPECT_TRUE(v.overlaps(top, 4));
    // A disjoint extent that does not wrap still answers false, so the two arms above are
    // about the arithmetic and not about the predicate having stopped discriminating.
    EXPECT_FALSE(v.overlaps(BASE + 64 * G, 4));
    EXPECT_TRUE(v.overlaps(BASE, 1));
    // And reserve inherits the refusal, which is where it matters: an admitted wrapped
    // extent is a mapping the list does not describe.
    EXPECT_FALSE(v.reserve(top, 4));
    EXPECT_FALSE(v.reserve(BASE + 64 * G, (SIZE_MAX / G) + 2u));
}

TEST(VRange, reserve_refuses_a_page_count_the_entry_cannot_hold)
{
    // VirtualRange::pages is narrower than the size_t reserve is handed, so the ceiling has
    // to be a refusal: truncating the count would store a shorter range than the caller
    // asked for, and every later overlap and coverage answer would be about that shorter one.
    // At a 4096-byte granule the byte count wraps first on a 32-bit host, so the arm runs
    // only where the ceiling is the binding limit.
    kickos::VirtualRanges v = made();
    if (sizeof(size_t) > 4)
    {
        size_t const over = kickos::VR_MAX_PAGES + 1u;
        EXPECT_FALSE(v.reserve(BASE, over));
        EXPECT_EQ(v.count(), 0u);
        EXPECT_FALSE(v.overlaps(BASE, 1));
    }
    // The ceiling itself is representable, which is what the static_assert in vrange.h says
    // and what makes the refusal above a bound rather than an off-by-one.
    EXPECT_EQ(static_cast<size_t>(static_cast<uint32_t>(kickos::VR_MAX_PAGES)),
              kickos::VR_MAX_PAGES);
    // A count the entry holds is still admitted, so the refusal is not blanket.
    EXPECT_TRUE(v.reserve(BASE, 4));
    EXPECT_EQ(v.count(), 1u);
}

TEST(VRange, grant_refuses_a_right_the_entry_cannot_hold)
{
    // The rights field is narrower than the uint32_t the caller passes. A bit above the
    // field would be dropped by a truncating store, and the entry would then read as
    // carrying fewer rights than were granted, so covers() would refuse an access the
    // mapping allows: a silent divergence between the record and the page tables.
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 2));
    uint32_t const too_wide = static_cast<uint32_t>(ARCH_MAP_R) | (1u << 16);
    EXPECT_FALSE(v.grant(BASE, 2, too_wide));
    // Still only reserved, so the refusal took nothing.
    EXPECT_FALSE(v.covers(BASE, G, ARCH_MAP_R));
    // Every right the vocabulary defines is still admitted, so the refusal is not blanket.
    EXPECT_TRUE(v.grant(BASE, 2, ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X));
    EXPECT_TRUE(v.covers(BASE, G, ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X));
}
