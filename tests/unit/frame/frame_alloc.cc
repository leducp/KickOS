// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/mem/frame.cc. The allocator needs no hardware and no kernel: it is a
// range, a granule and a bitmap, so it is gated here rather than waiting for a board.
//
// The granule is passed EXPLICITLY by every arm, never taken from a constant, because F7's
// freeze is that the figure is the arch's answer. An arm that hardcoded 4096 would still pass
// on a backend reporting 8 KiB while the allocator handed out overlapping frames.

#include <kickos/frame.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

namespace
{
    // The managed range is real memory, the bitmap living inside it. Over-aligned so a test
    // can use any granule up to this without the base failing the alignment check.
    constexpr size_t ARENA_BYTES = 256u * 1024u;
    alignas(64u * 1024u) unsigned char g_arena[ARENA_BYTES];

    uintptr_t arena_base() { return reinterpret_cast<uintptr_t>(g_arena); }
}

TEST(Frame, init_rejects_what_it_cannot_describe)
{
    kickos::FrameAllocator f;
    EXPECT_FALSE(f.init(0, ARENA_BYTES, 4096)) << "a zero base is not a range";
    EXPECT_FALSE(f.init(arena_base(), ARENA_BYTES, 0)) << "a zero granule";
    EXPECT_FALSE(f.init(arena_base(), ARENA_BYTES, 3000)) << "a granule that is not a power of two";
    EXPECT_FALSE(f.init(arena_base() + 1, ARENA_BYTES, 4096)) << "an unaligned base";
    EXPECT_FALSE(f.init(arena_base(), 0, 4096)) << "an empty range";
    // One frame is all bitmap and leaves nothing usable. REFUSED, not reported as capacity 0:
    // a caller cannot tell capacity 0 from ordinary exhaustion.
    EXPECT_FALSE(f.init(arena_base(), 4096, 4096)) << "a range with no usable frame";
}

TEST(Frame, the_bitmap_costs_frames_and_says_so)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    size_t const frames = ARENA_BYTES / 4096;
    EXPECT_LT(f.frames_total(), frames) << "the bitmap has to live somewhere in the range";
    EXPECT_EQ(f.frames_free(), f.frames_total()) << "a fresh range is entirely free";
    // The bitmap's own frames read as allocated, which is what stops alloc handing them out.
    EXPECT_TRUE(f.is_allocated(arena_base())) << "frame 0 holds the bitmap";
}

TEST(Frame, every_allocation_is_distinct_aligned_and_inside_the_range)
{
    kickos::FrameAllocator f;
    constexpr size_t granule = 4096;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, granule));

    std::set<uintptr_t> seen;
    for (size_t i = 0; i < f.frames_total(); i++)
    {
        uintptr_t const a = f.alloc();
        ASSERT_NE(a, 0u) << "exhausted after " << i << " of " << f.frames_total();
        EXPECT_EQ(a % granule, 0u) << "frame not granule-aligned";
        EXPECT_GE(a, arena_base());
        EXPECT_LT(a + granule, arena_base() + ARENA_BYTES + granule);
        EXPECT_TRUE(seen.insert(a).second) << "the same frame was handed out twice";
        EXPECT_TRUE(f.is_allocated(a));
    }
    EXPECT_EQ(f.frames_free(), 0u);
}

TEST(Frame, exhaustion_returns_zero_and_stays_exhausted)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    while (f.alloc() != 0)
    {
    }
    EXPECT_EQ(f.frames_free(), 0u);
    // Repeated: an allocator that returned 0 once and then a stale frame would pass a single
    // call and corrupt the second caller.
    EXPECT_EQ(f.alloc(), 0u);
    EXPECT_EQ(f.alloc(), 0u);
}

TEST(Frame, release_returns_the_frame_to_the_pool)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    std::vector<uintptr_t> all;
    for (uintptr_t a = f.alloc(); a != 0; a = f.alloc())
    {
        all.push_back(a);
    }
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(f.alloc(), 0u);

    uintptr_t const one = all.back();
    ASSERT_TRUE(f.release(one));
    EXPECT_FALSE(f.is_allocated(one));
    EXPECT_EQ(f.frames_free(), 1u);
    // The very next allocation must be able to reach it, or a released frame is lost.
    EXPECT_EQ(f.alloc(), one);
    EXPECT_EQ(f.frames_free(), 0u);
}

TEST(Frame, a_double_release_is_refused_and_changes_nothing)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    uintptr_t const a = f.alloc();
    ASSERT_NE(a, 0u);
    size_t const before = f.frames_free();

    ASSERT_TRUE(f.release(a));
    EXPECT_EQ(f.frames_free(), before + 1);
    // THE ARM THAT A FREE LIST CANNOT ANSWER. A second release must be refused, and above all
    // must not raise the free count: doing so would let the same frame be handed to two
    // owners, which is the worst outcome this subsystem has.
    EXPECT_FALSE(f.release(a)) << "a double release was accepted";
    EXPECT_EQ(f.frames_free(), before + 1) << "a refused release still moved the count";
    // And the frame is still handed out exactly once afterwards.
    EXPECT_EQ(f.alloc(), a);
    EXPECT_FALSE(f.release(a + 1)) << "an interior address is not a frame";
}

TEST(Frame, release_refuses_what_it_never_handed_out)
{
    kickos::FrameAllocator f;
    constexpr size_t granule = 4096;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, granule));
    EXPECT_FALSE(f.release(arena_base())) << "the bitmap's own frame";
    EXPECT_FALSE(f.release(arena_base() - granule)) << "below the range";
    EXPECT_FALSE(f.release(arena_base() + ARENA_BYTES)) << "one past the range";
    EXPECT_FALSE(f.release(arena_base() + granule + 8)) << "unaligned";
    uintptr_t const a = f.alloc();
    ASSERT_NE(a, 0u);
    EXPECT_FALSE(f.release(a + granule)) << "a neighbouring frame that is not allocated";
}

// The granule is the arch's answer, so the allocator must be correct at more than one value.
class FrameGranule : public ::testing::TestWithParam<size_t>
{
};

TEST_P(FrameGranule, allocates_the_whole_range_at_any_granule)
{
    size_t const granule = GetParam();
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, granule)) << "granule " << granule;

    std::set<uintptr_t> seen;
    for (uintptr_t a = f.alloc(); a != 0; a = f.alloc())
    {
        EXPECT_EQ(a % granule, 0u);
        ASSERT_TRUE(seen.insert(a).second) << "duplicate at granule " << granule;
    }
    EXPECT_EQ(seen.size(), f.frames_total());
    EXPECT_EQ(f.frames_free(), 0u);
    // Give it all back, then take it all again: the accounting has to survive a full cycle.
    for (uintptr_t a : seen)
    {
        EXPECT_TRUE(f.release(a));
    }
    EXPECT_EQ(f.frames_free(), f.frames_total());
    size_t again = 0;
    while (f.alloc() != 0)
    {
        again++;
    }
    EXPECT_EQ(again, f.frames_total());
}

// 8 KiB is in the list on purpose: SPARC v9's smallest page is 8 KiB, which is F7's own
// reason for the granule being asked rather than believed.
INSTANTIATE_TEST_SUITE_P(Granules, FrameGranule,
                         ::testing::Values(size_t(1024), size_t(4096), size_t(8192),
                                           size_t(16384), size_t(65536)));
