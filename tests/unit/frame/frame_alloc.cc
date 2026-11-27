// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/mem/frame.cc.
//
// The granule is passed explicitly by every arm, never taken from a constant: the figure
// is the arch's answer and never a kernel constant. An arm that hardcoded 4096 would still
// pass on a backend reporting 8 KiB while the allocator handed out overlapping frames.

#include <kickos/frame.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

namespace
{
    // The managed range is real memory, the record living inside it. Over-aligned so a test
    // can use any granule up to this without the base failing the alignment check.
    constexpr size_t ARENA_BYTES = 256u * 1024u;
    alignas(64u * 1024u) unsigned char g_arena[ARENA_BYTES];

    uintptr_t arena_base() { return reinterpret_cast<uintptr_t>(g_arena); }

    // Written from sizeof rather than 64 so the arms below name the same frames on a 32-bit
    // host as on this one.
    constexpr size_t WORD_FRAMES = 8u * sizeof(size_t);

    size_t frame_of(uintptr_t addr, size_t granule)
    {
        return static_cast<size_t>((addr - arena_base()) / granule);
    }

    void fill(kickos::FrameAllocator& f)
    {
        while (f.alloc() != 0)
        {
        }
    }

    void release_span(kickos::FrameAllocator& f, size_t first, size_t count, size_t granule)
    {
        for (size_t i = first; i < first + count; i++)
        {
            ASSERT_TRUE(f.release(arena_base() + i * granule)) << "frame " << i;
        }
    }
}

TEST(Frame, init_rejects_what_it_cannot_describe)
{
    kickos::FrameAllocator f;
    EXPECT_FALSE(f.init(0, ARENA_BYTES, 4096)) << "a zero base is not a range";
    EXPECT_FALSE(f.init(arena_base(), ARENA_BYTES, 0)) << "a zero granule";
    EXPECT_FALSE(f.init(arena_base(), ARENA_BYTES, 3000)) << "a granule that is not a power of two";
    EXPECT_FALSE(f.init(arena_base(), ARENA_BYTES, sizeof(size_t))) << "too small for the links";
    EXPECT_FALSE(f.init(arena_base() + 1, ARENA_BYTES, 4096)) << "an unaligned base";
    EXPECT_FALSE(f.init(arena_base(), 0, 4096)) << "an empty range";
    // One frame is all record and leaves nothing usable. Refused, not reported as capacity
    // 0: a caller cannot tell capacity 0 from ordinary exhaustion.
    EXPECT_FALSE(f.init(arena_base(), 4096, 4096)) << "a range with no usable frame";
}

TEST(Frame, the_record_costs_frames_and_says_so)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    size_t const frames = ARENA_BYTES / 4096;
    EXPECT_LT(f.frames_total(), frames) << "the record has to live somewhere in the range";
    EXPECT_EQ(f.frames_free(), f.frames_total()) << "a fresh range is entirely free";
    // The record's own frames read as allocated, which is what stops alloc handing them out.
    EXPECT_TRUE(f.is_allocated(arena_base() + (frames - 1u) * 4096u)) << "the last frame holds it";
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
    // A second release must be refused, and above all must not raise the free count: doing
    // so would let the same frame be handed to two owners.
    EXPECT_FALSE(f.release(a)) << "a double release was accepted";
    EXPECT_EQ(f.frames_free(), before + 1) << "a refused release still moved the count";
    EXPECT_EQ(f.alloc(), a);
    EXPECT_FALSE(f.release(a + 1)) << "an interior address is not a frame";
}

TEST(Frame, release_refuses_what_it_never_handed_out)
{
    kickos::FrameAllocator f;
    constexpr size_t granule = 4096;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, granule));
    EXPECT_FALSE(f.release(arena_base() + ARENA_BYTES - granule)) << "the record's own frame";
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

TEST(Frame, a_run_is_contiguous_and_wholly_allocated)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    size_t const before = f.frames_free();
    uintptr_t const run = f.alloc_run(4);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(f.frames_free(), before - 4u) << "a run of four costs four frames";
    for (size_t i = 0; i < 4; i++)
    {
        EXPECT_TRUE(f.is_allocated(run + i * 4096u)) << "frame " << i << " of the run";
    }
    // The point of a run: the caller reaches every page of it through one address, so the
    // frames have to be consecutive and not merely four of them.
    EXPECT_FALSE(f.is_allocated(run + 4u * 4096u)) << "the run is exactly four frames wide";
    // Released one at a time, which is the whole of the accounting: no run-shaped free.
    for (size_t i = 0; i < 4; i++)
    {
        EXPECT_TRUE(f.release(run + i * 4096u));
    }
    EXPECT_EQ(f.frames_free(), before);
}

TEST(Frame, a_run_longer_than_the_range_is_refused)
{
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    size_t const total = f.frames_total();
    EXPECT_EQ(f.alloc_run(0), 0u) << "an empty run is not a request";
    EXPECT_EQ(f.alloc_run(total + 1u), 0u) << "longer than the range holds";
    EXPECT_EQ(f.frames_free(), total) << "a refused run takes nothing";
    // The whole usable range as one run, which the record's own frames must not break.
    uintptr_t const all = f.alloc_run(total);
    ASSERT_NE(all, 0u);
    EXPECT_EQ(f.frames_free(), 0u);
}

TEST(Frame, a_run_is_refused_by_FRAGMENTATION_and_not_by_the_free_count)
{
    // The count says a run fits and the layout says it does not.
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), ARENA_BYTES, 4096));
    std::vector<uintptr_t> all;
    while (true)
    {
        uintptr_t const p = f.alloc();
        if (p == 0)
        {
            break;
        }
        all.push_back(p);
    }
    ASSERT_GE(all.size(), 8u);
    // Every other frame back, so half the pool is free and no two free frames are adjacent.
    for (size_t i = 0; i < all.size(); i += 2)
    {
        ASSERT_TRUE(f.release(all[i]));
    }
    ASSERT_GE(f.frames_free(), 2u);
    EXPECT_EQ(f.alloc_run(2), 0u) << "no two adjacent frames, though the count allows a run";
    EXPECT_NE(f.alloc(), 0u) << "a single frame is still available";
}

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

TEST(Frame, a_whole_free_word_is_not_one_frame_longer_than_it_is)
{
    // The gap is a word wide and word-aligned, with the frames either side of it taken, and
    // the free count allows a request one frame longer.
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, WORD_FRAMES, WORD_FRAMES, granule);
    release_span(f, 3u * WORD_FRAMES + 5u, 1u, granule);
    release_span(f, 3u * WORD_FRAMES + 7u, 1u, granule);
    ASSERT_EQ(f.frames_free(), WORD_FRAMES + 2u);
    EXPECT_EQ(f.alloc_run(WORD_FRAMES + 2u), 0u);
    EXPECT_EQ(f.alloc_run(WORD_FRAMES + 1u), 0u) << "the gap is exactly one word";
    uintptr_t const run = f.alloc_run(WORD_FRAMES);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, granule), WORD_FRAMES);
}

TEST(Frame, a_lone_free_frame_anywhere_is_found_again)
{
    constexpr size_t granule = 1024;
    size_t const singles[] = {0u, 1u, WORD_FRAMES - 1u, WORD_FRAMES, WORD_FRAMES + 1u,
                              2u * WORD_FRAMES - 1u, 2u * WORD_FRAMES, 254u};
    for (size_t idx : singles)
    {
        kickos::FrameAllocator f;
        ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
        fill(f);
        release_span(f, idx, 1u, granule);
        uintptr_t const a = f.alloc();
        ASSERT_NE(a, 0u) << "frame " << idx << " was freed and could not be found again";
        EXPECT_EQ(frame_of(a, granule), idx);
        EXPECT_EQ(f.frames_free(), 0u);
    }
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, WORD_FRAMES - 1u, 2u, granule);
    std::set<size_t> back;
    back.insert(frame_of(f.alloc(), granule));
    back.insert(frame_of(f.alloc(), granule));
    EXPECT_EQ(back, (std::set<size_t>{WORD_FRAMES - 1u, WORD_FRAMES}));
    EXPECT_EQ(f.alloc(), 0u);
}

TEST(Frame, a_frame_count_that_is_not_a_power_of_two_hands_out_no_frame_past_the_end)
{
    // The record takes the last frames, so a frame past the usable ones is the record's and
    // one past the range is not memory the caller gave.
    size_t const granules[] = {1024u, 4096u, 16u};
    size_t const counts[] = {65u, 66u, 67u, 70u, 71u, 127u, 129u, 130u, 200u};
    for (size_t granule : granules)
    {
        for (size_t count : counts)
        {
            if (count * granule > ARENA_BYTES)
            {
                continue;
            }
            kickos::FrameAllocator f;
            ASSERT_TRUE(f.init(arena_base(), count * granule, granule))
                << "granule " << granule << " count " << count;
            size_t const usable = f.frames_total();
            ASSERT_LT(usable, count);
            size_t handed = 0;
            size_t top = 0;
            for (uintptr_t a = f.alloc(); a != 0; a = f.alloc())
            {
                size_t const i = frame_of(a, granule);
                ASSERT_LT(i, usable) << "granule " << granule << " count " << count
                                     << ": a frame of the record or past the range";
                if (i > top)
                {
                    top = i;
                }
                handed++;
            }
            EXPECT_EQ(handed, usable) << "granule " << granule << " count " << count;
            EXPECT_EQ(top, usable - 1u) << "the last usable frame was never handed out";
            EXPECT_EQ(f.alloc(), 0u);
            EXPECT_EQ(f.alloc_run(1), 0u);
            EXPECT_TRUE(f.is_allocated(arena_base() + (count - 1u) * granule)) << "the record";
            release_span(f, usable - 1u, 1u, granule);
            EXPECT_EQ(frame_of(f.alloc(), granule), usable - 1u);
        }
    }
}

TEST(Frame, a_run_does_not_span_a_wholly_allocated_word)
{
    // Free frames on either side of a whole allocated word are not one run.
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, WORD_FRAMES - 1u, 1u, granule);
    release_span(f, 2u * WORD_FRAMES, 1u, granule);
    ASSERT_EQ(f.frames_free(), 2u);
    EXPECT_EQ(f.alloc_run(2), 0u) << "two free frames a whole word apart are not a run";
    EXPECT_EQ(f.frames_free(), 2u) << "a refused run took frames anyway";
    EXPECT_TRUE(f.is_allocated(arena_base() + (WORD_FRAMES - 2u) * granule))
        << "the frame below the free one was handed out";
    kickos::FrameAllocator g;
    ASSERT_TRUE(g.init(arena_base(), 256u * granule, granule));
    fill(g);
    release_span(g, WORD_FRAMES - 4u, 4u, granule);
    release_span(g, 2u * WORD_FRAMES, 4u, granule);
    EXPECT_EQ(g.alloc_run(5), 0u) << "four plus four across an allocated word is not five";
    EXPECT_EQ(g.frames_free(), 8u);
    uintptr_t const run = g.alloc_run(4);
    ASSERT_NE(run, 0u);
    size_t const lo = frame_of(run, granule);
    EXPECT_TRUE(lo == WORD_FRAMES - 4u or lo == 2u * WORD_FRAMES) << "run at " << lo;
}

// --- the buddy system ----------------------------------------------------------------------

namespace
{
    // 256 frames of 1 KiB: the record takes frame 255, and the free frames below it are one
    // block of each order from 7 at frame 0 down to 0 at frame 254.
    constexpr size_t G = 1024u;
    constexpr size_t COUNT = 256u;
    constexpr size_t USABLE = 255u;

    uintptr_t at(size_t i)
    {
        return arena_base() + i * G;
    }

    void init_shape(kickos::FrameAllocator& f)
    {
        ASSERT_TRUE(f.init(arena_base(), COUNT * G, G));
        ASSERT_EQ(f.frames_total(), USABLE) << "fixture: one frame of record";
    }

    // Every frame alloc() still hands out.
    std::set<size_t> drain(kickos::FrameAllocator& f)
    {
        std::set<size_t> got;
        for (uintptr_t a = f.alloc(); a != 0; a = f.alloc())
        {
            got.insert(frame_of(a, G));
        }
        return got;
    }
}

TEST(Buddy, a_run_takes_the_smallest_block_that_holds_it_and_gives_the_tail_back)
{
    kickos::FrameAllocator f;
    init_shape(f);
    uintptr_t const run = f.alloc_run(5);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, G), 240u) << "the block of eight, not a larger one";
    for (size_t i = 0; i < 5u; i++)
    {
        EXPECT_TRUE(f.is_allocated(run + i * G)) << "frame " << i << " of the run";
    }
    for (size_t i = 5u; i < 8u; i++)
    {
        EXPECT_FALSE(f.is_allocated(run + i * G)) << "frame " << i << " of the block's tail";
    }
    EXPECT_EQ(f.frames_free(), USABLE - 5u);
    std::set<size_t> const got = drain(f);
    EXPECT_EQ(got.size(), USABLE - 5u) << "a frame the count promises is not reachable";
    EXPECT_EQ(got.count(245u) + got.count(246u) + got.count(247u), 3u) << "the tail was lost";
}

TEST(Buddy, a_run_that_is_not_a_power_of_two_takes_exactly_its_pages)
{
    kickos::FrameAllocator f;
    init_shape(f);
    fill(f);
    // An aligned block of eight and one of four: a run of five fits only the eight.
    release_span(f, 16u, 8u, G);
    release_span(f, 32u, 4u, G);
    uintptr_t const run = f.alloc_run(5);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, G), 16u);
    EXPECT_EQ(f.frames_free(), 7u);
    for (size_t i = 21u; i < 24u; i++)
    {
        EXPECT_FALSE(f.is_allocated(at(i))) << "frame " << i;
    }
    EXPECT_EQ(f.alloc_run(3), at(32u)) << "the block of four";
    EXPECT_EQ(f.alloc_run(2), at(22u)) << "the tail of the eight";
    EXPECT_EQ(f.frames_free(), 2u);
    EXPECT_EQ(drain(f), (std::set<size_t>{21u, 35u}));
}

TEST(Buddy, a_released_block_merges_with_its_free_buddy)
{
    kickos::FrameAllocator f;
    init_shape(f);
    fill(f);
    // The 64 frames of one block, back one at a time in an order that leaves each merge
    // to the last of its pair.
    for (size_t k = 0; k < 64u; k++)
    {
        size_t const i = (k * 37u) % 64u;
        ASSERT_TRUE(f.release(at(i))) << "frame " << i;
    }
    EXPECT_EQ(f.alloc_run(64), at(0)) << "64 free frames that are not one block";
    EXPECT_EQ(f.frames_free(), 0u);
    ASSERT_EQ(f.release_run(at(0), 64), 64u);
    ASSERT_EQ(f.release_run(at(64), 64), 64u);
    EXPECT_EQ(f.alloc_run(128), at(0)) << "two released runs of 64 that are not one block";
}

TEST(Buddy, a_run_longer_than_any_free_block_starts_at_the_first_frame_and_stops_at_the_record)
{
    kickos::FrameAllocator f;
    init_shape(f);
    uintptr_t const all = f.alloc_run(USABLE);
    ASSERT_EQ(all, at(0)) << "the whole usable range, longer than its largest block";
    EXPECT_EQ(f.frames_free(), 0u);
    EXPECT_EQ(f.release_run(all, COUNT), 0u) << "a release reaching the record";
    EXPECT_FALSE(f.release(at(USABLE))) << "the record's frame";
    EXPECT_TRUE(f.is_allocated(at(USABLE)));
    EXPECT_EQ(f.release_run(all, USABLE), USABLE);
    EXPECT_EQ(f.frames_free(), USABLE);
    // With frame 0 taken a run that long is refused, though the 254 frames above are free.
    ASSERT_EQ(f.alloc_run(USABLE), at(0));
    ASSERT_EQ(f.release_run(at(1), USABLE - 1u), USABLE - 1u);
    EXPECT_EQ(f.alloc_run(USABLE - 1u), 0u) << "the run is only ever tried at frame 0";
    EXPECT_EQ(f.frames_free(), USABLE - 1u) << "a refused run took frames anyway";
}

TEST(Buddy, a_run_in_the_blocks_the_record_cuts_short_ends_at_the_last_usable_frame)
{
    // 67 frames: one block of 64, one of two, then the record, whose buddy is past the range.
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 67u * G, G));
    ASSERT_EQ(f.frames_total(), 66u) << "fixture: one frame of record";
    uintptr_t const end = f.alloc_run(2);
    EXPECT_EQ(end, at(64u)) << "the block of two at the top";
    EXPECT_TRUE(f.is_allocated(at(66u))) << "the record";
    EXPECT_EQ(f.release_run(end, 3), 0u) << "a release reaching the record";
    EXPECT_EQ(f.release_run(end, 2), 2u);
    EXPECT_EQ(f.alloc_run(66), at(0)) << "the whole usable range, ending at frame 65";
    EXPECT_EQ(f.release_run(at(0), 66), 66u);
    EXPECT_EQ(f.frames_free(), 66u);
}

TEST(Buddy, a_block_ending_at_the_range_s_end_never_merges_past_it)
{
    // 24 frames, 8 * 3: the allocated block [16, 24) has the buddy index 24, which names the
    // byte right after the tags, the low byte of the order-0 list's head. That head is made
    // frame 4, whose low byte is the tag of an allocated block of order 3.
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 24u * G, G));
    ASSERT_EQ(f.frames_total(), 23u) << "fixture: one frame of record";
    fill(f);
    ASSERT_TRUE(f.release(at(18u)));
    ASSERT_TRUE(f.release(at(19u)));
    ASSERT_TRUE(f.release(at(4u)));
    EXPECT_EQ(f.alloc_run(2), at(18u)) << "fixture: the block of two closing the tail";
    EXPECT_EQ(f.alloc(), at(4u)) << "the order-0 list lost its head";
    EXPECT_EQ(f.alloc(), 0u);
    EXPECT_EQ(f.release_run(at(16u), 8u), 0u) << "a release reaching the record";
    EXPECT_EQ(f.release_run(at(16u), 7u), 7u);
    EXPECT_EQ(f.frames_free(), 7u);
}

TEST(Buddy, a_release_reaching_a_free_frame_is_refused_whole)
{
    kickos::FrameAllocator f;
    init_shape(f);
    uintptr_t const run = f.alloc_run(5);
    ASSERT_NE(run, 0u);
    size_t const free_before = f.frames_free();
    ASSERT_TRUE(f.release(run + 2u * G));
    EXPECT_EQ(f.release_run(run, 5), 0u) << "a run holding a free frame";
    EXPECT_EQ(f.frames_free(), free_before + 1u) << "a refused release still moved the count";
    for (size_t i : {0u, 1u, 3u, 4u})
    {
        EXPECT_TRUE(f.is_allocated(run + i * G)) << "frame " << i << " freed by a refusal";
    }
    EXPECT_FALSE(f.release(run + 2u * G)) << "a double release";
    EXPECT_EQ(f.release_run(run + 3u * G, 2), 2u);
    EXPECT_EQ(f.release_run(run, 2), 2u);
    EXPECT_EQ(f.release_run(run, 2), 0u) << "a second release of a run";
    EXPECT_EQ(f.frames_free(), USABLE);
    EXPECT_EQ(drain(f).size(), USABLE) << "a refused release corrupted the free lists";
}

TEST(Buddy, exhaustion_and_fragmentation_refuse_without_taking_anything)
{
    kickos::FrameAllocator f;
    init_shape(f);
    fill(f);
    EXPECT_EQ(f.alloc(), 0u);
    EXPECT_EQ(f.alloc_run(1), 0u);
    EXPECT_EQ(f.alloc_run(USABLE), 0u);
    EXPECT_EQ(f.frames_free(), 0u);
    for (size_t i = 0; i < USABLE; i += 2u)
    {
        ASSERT_TRUE(f.release(at(i)));
    }
    size_t const freed = f.frames_free();
    EXPECT_EQ(f.alloc_run(2), 0u) << "no two adjacent frames, though the count allows a run";
    EXPECT_EQ(f.alloc_run(freed), 0u);
    EXPECT_EQ(f.frames_free(), freed) << "a refused run took frames anyway";
    for (size_t i = 0; i < USABLE; i++)
    {
        EXPECT_EQ(f.is_allocated(at(i)), i % 2u == 1u) << "frame " << i;
    }
    std::set<size_t> const got = drain(f);
    EXPECT_EQ(got.size(), freed);
    EXPECT_EQ(*got.rbegin(), 254u);
}

TEST(Buddy, a_table_reserve_taken_and_given_back_leaves_the_pool_as_it_was)
{
    // The presync reserve's pattern: single frames chained through their first word, then
    // released down the chain.
    kickos::FrameAllocator f;
    init_shape(f);
    for (int round = 0; round < 3; round++)
    {
        uintptr_t chain = 0;
        for (size_t k = 0; k < 32u; k++)
        {
            uintptr_t const a = f.alloc();
            ASSERT_NE(a, 0u);
            *reinterpret_cast<uintptr_t*>(a) = chain;
            chain = a;
        }
        EXPECT_EQ(f.frames_free(), USABLE - 32u);
        while (chain != 0)
        {
            uintptr_t const next = *reinterpret_cast<uintptr_t*>(chain);
            ASSERT_TRUE(f.release(chain));
            chain = next;
        }
        EXPECT_EQ(f.frames_free(), USABLE) << "round " << round;
        uintptr_t const all = f.alloc_run(USABLE);
        ASSERT_EQ(all, at(0)) << "the reserve left the pool cut up, round " << round;
        ASSERT_EQ(f.release_run(all, USABLE), USABLE);
    }
}

TEST(Frame, an_interleaved_sequence_hands_no_frame_out_twice)
{
    // A shadow of every frame's state kept beside the allocator, checked after every call:
    // over a long mixed sequence the allocator must never hand out a frame it already gave
    // away, never refuse a run a free aligned block of its power of two holds, and agree with
    // the shadow on every frame.
    size_t const granules[] = {1024u, 4096u};
    size_t const counts[] = {67u, 200u, 64u};
    for (size_t granule : granules)
    {
        for (size_t count : counts)
        {
            if (count * granule > ARENA_BYTES)
            {
                continue;
            }
            kickos::FrameAllocator f;
            ASSERT_TRUE(f.init(arena_base(), count * granule, granule));
            size_t const usable = f.frames_total();
            std::vector<bool> taken(count, false);
            for (size_t i = usable; i < count; i++)
            {
                taken[i] = true;
            }
            auto all_free = [&](size_t lo, size_t n) {
                for (size_t i = lo; i < lo + n; i++)
                {
                    if (i >= count or taken[i])
                    {
                        return false;
                    }
                }
                return true;
            };
            uint64_t seed = 0x9E3779B97F4A7C15ull;
            for (size_t step = 0; step < 8000u; step++)
            {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                size_t const pick = static_cast<size_t>(seed % 100u);
                size_t const arg = static_cast<size_t>((seed >> 8) % (count + 2u));
                size_t const pages = 1u + static_cast<size_t>((seed >> 24) % 12u);
                if (pick < 40u)
                {
                    uintptr_t const a = f.alloc();
                    if (a == 0u)
                    {
                        for (size_t i = 0; i < count; i++)
                        {
                            ASSERT_TRUE(taken[i]) << "frame " << i << " free, alloc refused";
                        }
                        continue;
                    }
                    size_t const i = frame_of(a, granule);
                    ASSERT_LT(i, usable) << "step " << step;
                    ASSERT_FALSE(taken[i]) << "frame " << i << " handed out twice, step " << step;
                    taken[i] = true;
                }
                else if (pick < 60u)
                {
                    size_t order = 0;
                    while ((size_t(1) << order) < pages)
                    {
                        order++;
                    }
                    size_t const block = size_t(1) << order;
                    bool fits = all_free(0, pages);
                    for (size_t w = 0; w + block <= count; w += block)
                    {
                        fits = fits or all_free(w, block);
                    }
                    uintptr_t const a = f.alloc_run(pages);
                    if (a == 0u)
                    {
                        ASSERT_FALSE(fits) << "a run of " << pages << " refused, step " << step;
                        continue;
                    }
                    size_t const lo = frame_of(a, granule);
                    ASSERT_EQ(lo % block, 0u) << "a run off its alignment, step " << step;
                    ASSERT_TRUE(all_free(lo, pages)) << "a run over taken frames, step " << step;
                    for (size_t k = 0; k < pages; k++)
                    {
                        taken[lo + k] = true;
                    }
                }
                else if (pick < 75u)
                {
                    size_t const n = pages;
                    bool expect = arg + n <= usable;
                    for (size_t i = arg; expect and i < arg + n; i++)
                    {
                        expect = taken[i];
                    }
                    size_t want = 0;
                    if (expect)
                    {
                        want = n;
                    }
                    ASSERT_EQ(f.release_run(arena_base() + arg * granule, n), want)
                        << "release_run(" << arg << ", " << n << ") verdict, step " << step;
                    for (size_t i = arg; expect and i < arg + n; i++)
                    {
                        taken[i] = false;
                    }
                }
                else
                {
                    bool const expect = arg < usable and taken[arg];
                    ASSERT_EQ(f.release(arena_base() + arg * granule), expect)
                        << "release(" << arg << ") verdict, step " << step;
                    if (expect)
                    {
                        taken[arg] = false;
                    }
                }
                size_t free_now = 0;
                for (size_t i = 0; i < count; i++)
                {
                    if (not taken[i])
                    {
                        free_now++;
                    }
                    ASSERT_EQ(f.is_allocated(arena_base() + i * granule), taken[i])
                        << "frame " << i << ", step " << step;
                }
                ASSERT_EQ(f.frames_free(), free_now) << "the free count drifted, step " << step;
            }
        }
    }
}

// 8 KiB is in the list on purpose: SPARC v9's smallest page is 8 KiB, which is the reason
// the granule is asked rather than believed.
INSTANTIATE_TEST_SUITE_P(Granules, FrameGranule,
                         ::testing::Values(size_t(1024), size_t(4096), size_t(8192),
                                           size_t(16384), size_t(65536)));
