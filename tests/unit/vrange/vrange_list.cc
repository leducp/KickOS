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

TEST(VRange, find_answers_the_one_entry_a_range_lies_inside)
{
    // The identity question the self-grant asks: an address this list never reserved is in
    // no entry, which is what a cross-task self-grant now is (F10).
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 4));
    EXPECT_EQ(v.find(BASE - 1, 1), nullptr);
    EXPECT_EQ(v.find(BASE + 4 * G, 1), nullptr);
    EXPECT_EQ(v.find(BASE, 0), nullptr);
    kickos::VirtualRange const* const e = v.find(BASE + 7, 9);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->base, BASE);   // the ENTRY's own extent, not the caller's slice
    EXPECT_EQ(e->pages, 4u);
    // A RESERVATION ANSWERS TOO, unlike covers(): the self-grant's whole job is to find the
    // entry that has not been granted yet.
    EXPECT_EQ(e->state, kickos::VirtualState::Reserved);
    EXPECT_FALSE(v.covers(BASE, 1, ARCH_MAP_R));
}

TEST(VRange, find_refuses_a_range_leaving_the_entry_or_wrapping)
{
    kickos::VirtualRanges v = made();
    ASSERT_TRUE(v.reserve(BASE, 2));
    ASSERT_TRUE(v.reserve(BASE + 2 * G, 2)); // flush above, a SECOND entry
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
    // The already-mapped short-circuit asks whether the range carries THESE attributes, so a
    // second grant at another type has to be distinguishable from the first.
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
