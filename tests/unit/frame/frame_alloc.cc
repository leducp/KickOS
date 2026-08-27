// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host arms for kernel/mem/frame.cc.
//
// The granule is passed explicitly by every arm, never taken from a constant, because F7's
// freeze is that the figure is the arch's answer. An arm that hardcoded 4096 would still
// pass on a backend reporting 8 KiB while the allocator handed out overlapping frames.

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

    // The scan reads the bitmap a size_t at a time, so the frames that share a word with a
    // boundary, and the frames of the last partial word, are the ones a scan gets wrong.
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

    // Zeroes the whole arena before init(). A zero bit reads as free, so every byte the
    // bitmap does not cover, and every bit of its last byte past the last frame, is left
    // saying "available". A scan that reads one of them hands out a frame outside the range
    // the caller gave; with residue from an earlier arm in there instead, the same defect
    // reads as a pass.
    void poison_free(size_t bytes)
    {
        for (size_t i = 0; i < bytes; i++)
        {
            g_arena[i] = 0;
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
    EXPECT_FALSE(f.init(arena_base() + 1, ARENA_BYTES, 4096)) << "an unaligned base";
    EXPECT_FALSE(f.init(arena_base(), 0, 4096)) << "an empty range";
    // One frame is all bitmap and leaves nothing usable. Refused, not reported as capacity
    // 0: a caller cannot tell capacity 0 from ordinary exhaustion.
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
    // The whole usable range as one run, which the bitmap's own frames must not break.
    uintptr_t const all = f.alloc_run(total);
    ASSERT_NE(all, 0u);
    EXPECT_EQ(f.frames_free(), 0u);
}

TEST(Frame, a_run_is_refused_by_FRAGMENTATION_and_not_by_the_free_count)
{
    // The count says a run fits and the layout says it does not, which is why alloc_run may
    // not start from the single-allocation hint.
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

// --- the frames a word-wise scan gets wrong ----------------------------------------------

TEST(Frame, a_run_straddles_a_word_boundary)
{
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    // Five frames across the first word boundary, and two more far above so the refusal
    // below is the layout's and not the free count's.
    release_span(f, WORD_FRAMES - 2u, 5u, granule);
    release_span(f, 2u * WORD_FRAMES + 10u, 2u, granule);
    ASSERT_EQ(f.frames_free(), 7u);
    EXPECT_EQ(f.alloc_run(6), 0u) << "no six adjacent frames, though seven are free";
    uintptr_t const run = f.alloc_run(5);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, granule), WORD_FRAMES - 2u);
    for (size_t i = 0; i < 5u; i++)
    {
        EXPECT_TRUE(f.is_allocated(run + i * granule)) << "frame " << i << " of the run";
    }
}

TEST(Frame, a_run_straddles_two_word_boundaries)
{
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    size_t const first = WORD_FRAMES - 1u;
    size_t const len = WORD_FRAMES + 3u; // reaches into the third word
    release_span(f, first, len, granule);
    release_span(f, 3u * WORD_FRAMES + 20u, 2u, granule);
    ASSERT_EQ(f.frames_free(), len + 2u);
    EXPECT_EQ(f.alloc_run(len + 2u), 0u);
    EXPECT_EQ(f.alloc_run(len + 1u), 0u) << "one frame longer than the only gap";
    uintptr_t const run = f.alloc_run(len);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, granule), first);
    EXPECT_EQ(f.frames_free(), 2u);
}

TEST(Frame, a_whole_free_word_is_not_one_frame_longer_than_it_is)
{
    // The gap is a word wide and word-aligned, with the frames either side of it taken. A
    // scan that skipped the free word without stopping at its end would answer a request one
    // frame too long, and the free count allows that request.
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

TEST(Frame, the_last_frame_of_a_word_and_the_first_of_the_next_are_both_reachable)
{
    constexpr size_t granule = 1024;
    size_t const singles[] = {WORD_FRAMES - 1u, WORD_FRAMES, WORD_FRAMES + 1u,
                              2u * WORD_FRAMES - 1u, 2u * WORD_FRAMES};
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
    // Both sides of one boundary free at once: the lower one has to come out first, or the
    // scan is not first-fit.
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, WORD_FRAMES - 1u, 2u, granule);
    EXPECT_EQ(frame_of(f.alloc(), granule), WORD_FRAMES - 1u);
    EXPECT_EQ(frame_of(f.alloc(), granule), WORD_FRAMES);
}

TEST(Frame, a_frame_count_that_is_not_a_word_multiple_hands_out_no_frame_past_the_end)
{
    // The bitmap is rounded up to whole bytes, so bits past the last frame exist and are
    // zero. A word-wise scan sees them, and handing one out would return an address outside
    // the range the caller gave.
    size_t const granules[] = {1024u, 4096u, 8u};
    size_t const counts[] = {65u, 66u, 67u, 70u, 71u, 127u, 129u, 130u, 200u};
    for (size_t granule : granules)
    {
        for (size_t count : counts)
        {
            if (count * granule > ARENA_BYTES)
            {
                continue;
            }
            poison_free(count * granule);
            kickos::FrameAllocator f;
            ASSERT_TRUE(f.init(arena_base(), count * granule, granule))
                << "granule " << granule << " count " << count;
            size_t const reserved = count - f.frames_total();
            size_t handed = 0;
            size_t top = 0;
            for (uintptr_t a = f.alloc(); a != 0; a = f.alloc())
            {
                size_t const i = frame_of(a, granule);
                ASSERT_LT(i, count) << "granule " << granule << " count " << count
                                    << ": frame past the last one the range holds";
                ASSERT_GE(i, reserved);
                if (i > top)
                {
                    top = i;
                }
                handed++;
            }
            EXPECT_EQ(handed, f.frames_total()) << "granule " << granule << " count " << count;
            EXPECT_EQ(top, count - 1u) << "the last frame of the range was never handed out";
            EXPECT_EQ(f.alloc(), 0u);
            EXPECT_EQ(f.alloc_run(1), 0u);
            // And the last frame comes back, from inside the partial word.
            release_span(f, count - 1u, 1u, granule);
            EXPECT_EQ(frame_of(f.alloc(), granule), count - 1u);
        }
    }
}

TEST(Frame, a_run_ending_in_the_last_partial_word_is_found_and_not_overrun)
{
    // 67 frames is neither a word nor a byte multiple, so the run below sits in a word that
    // is part frames, part byte rounding, part nothing at all.
    constexpr size_t granule = 1024;
    constexpr size_t count = 67;
    poison_free(count * granule);
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), count * granule, granule));
    fill(f);
    release_span(f, count - 3u, 3u, granule);
    // Two more free frames, isolated, far below: without them the free count refuses the
    // run of four before any scan runs, and the scan is what is on trial here.
    release_span(f, 10u, 1u, granule);
    release_span(f, 12u, 1u, granule);
    ASSERT_EQ(f.frames_free(), 5u);
    EXPECT_EQ(f.alloc_run(4), 0u) << "a fourth frame would be past the end of the range";
    EXPECT_EQ(f.frames_free(), 5u) << "a refused run took frames anyway";
    uintptr_t const run = f.alloc_run(3);
    ASSERT_NE(run, 0u);
    EXPECT_EQ(frame_of(run, granule), count - 3u);
    EXPECT_EQ(f.frames_free(), 2u);
}

TEST(Frame, alloc_wraps_to_a_free_frame_below_the_hint)
{
    // alloc_run raises the hint over a lower free frame, which is the only way the hint can
    // sit above one: release() only ever moves it down. The second pass is what finds it,
    // and both the frame and the hint are chosen so the pass boundaries fall inside a word.
    constexpr size_t granule = 1024;
    size_t const lows[] = {1u, 2u, WORD_FRAMES - 1u, WORD_FRAMES, WORD_FRAMES + 1u,
                           2u * WORD_FRAMES - 1u};
    for (size_t low : lows)
    {
        kickos::FrameAllocator f;
        ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
        size_t const reserved = 256u - f.frames_total();
        if (low < reserved)
        {
            continue;
        }
        fill(f);
        release_span(f, low, 1u, granule);
        release_span(f, 130u, 11u, granule); // the run the hint follows, ending mid-word
        ASSERT_EQ(f.frames_free(), 12u);
        uintptr_t const run = f.alloc_run(11);
        ASSERT_NE(run, 0u);
        ASSERT_EQ(frame_of(run, granule), 130u) << "the single free frame is not a run of 11";
        // Everything at or above the hint is taken, so only the wrap pass can answer.
        uintptr_t const a = f.alloc();
        ASSERT_NE(a, 0u) << "the free frame below the hint was lost, low = " << low;
        EXPECT_EQ(frame_of(a, granule), low);
        EXPECT_EQ(f.frames_free(), 0u);
        EXPECT_EQ(f.alloc(), 0u);
    }
}

TEST(Frame, a_run_whose_fit_begins_below_the_hint_is_still_found)
{
    // The property the sweep's start at the bitmap's own frames protects: the hint chases
    // single allocations and says nothing about where a run fits.
    constexpr size_t granule = 1024;
    size_t const gaps[] = {10u, WORD_FRAMES - 2u, WORD_FRAMES, 2u * WORD_FRAMES - 3u};
    for (size_t gap : gaps)
    {
        kickos::FrameAllocator f;
        ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
        fill(f);
        release_span(f, gap, 4u, granule);
        release_span(f, 200u, 8u, granule);
        uintptr_t const high = f.alloc_run(8);
        ASSERT_NE(high, 0u);
        ASSERT_EQ(frame_of(high, granule), 200u) << "the four-frame gap is not a run of eight";
        // The hint is now above 207 and the only remaining fit is at `gap`.
        uintptr_t const run = f.alloc_run(4);
        ASSERT_NE(run, 0u) << "a run below the hint was refused, gap at " << gap;
        EXPECT_EQ(frame_of(run, granule), gap);
        EXPECT_EQ(f.frames_free(), 0u);
    }
}

TEST(Frame, a_run_does_not_span_a_wholly_allocated_word)
{
    // A free frame at the top of one word and another at the bottom of the word after next,
    // with a whole allocated word between them. A scan that skips the allocated word without
    // dropping what it had counted joins the two into a run of two, and the frame it would
    // return is allocated.
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
    EXPECT_EQ(frame_of(f.alloc(), granule), WORD_FRAMES - 1u);
    EXPECT_EQ(frame_of(f.alloc(), granule), 2u * WORD_FRAMES);
    // And the same shape with the free frames wide enough to be a run on each side.
    kickos::FrameAllocator g;
    ASSERT_TRUE(g.init(arena_base(), 256u * granule, granule));
    fill(g);
    release_span(g, WORD_FRAMES - 3u, 3u, granule);
    release_span(g, 2u * WORD_FRAMES, 3u, granule);
    EXPECT_EQ(g.alloc_run(4), 0u) << "three plus three across an allocated word is not four";
    EXPECT_EQ(frame_of(g.alloc_run(3), granule), WORD_FRAMES - 3u);
}

TEST(Frame, the_wrap_pass_does_not_return_a_frame_the_first_pass_walked_past)
{
    // Two free frames, both below the hint, one of them in the hint's own word. The first
    // pass starts inside that word and must ignore everything below the hint, or it answers
    // with the higher frame and first-fit is gone.
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, 10u, 1u, granule);
    release_span(f, 2u * WORD_FRAMES + 7u, 1u, granule);
    release_span(f, 2u * WORD_FRAMES + 13u, 11u, granule);
    uintptr_t const run = f.alloc_run(11);
    ASSERT_NE(run, 0u);
    ASSERT_EQ(frame_of(run, granule), 2u * WORD_FRAMES + 13u);
    // The hint now sits in the same word as the free frame at 2 * WORD_FRAMES + 7, above it.
    uintptr_t const a = f.alloc();
    ASSERT_NE(a, 0u);
    EXPECT_EQ(frame_of(a, granule), 10u) << "the lowest free frame is the first fit";
    EXPECT_EQ(frame_of(f.alloc(), granule), 2u * WORD_FRAMES + 7u);
}

TEST(Frame, taking_a_run_below_the_hint_leaves_the_hint_where_it_was)
{
    // The hint is a floor for single allocations, and a run taken from below it must not drag
    // it down: the next alloc() would then re-walk frames the hint had already accounted for
    // and answer with a lower frame than first-fit-from-the-hint gives.
    constexpr size_t granule = 1024;
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), 256u * granule, granule));
    fill(f);
    release_span(f, 10u, 4u, granule);
    release_span(f, 50u, 1u, granule);
    release_span(f, 250u, 1u, granule);
    release_span(f, 200u, 8u, granule);
    ASSERT_EQ(f.alloc_run(8), arena_base() + 200u * granule);
    ASSERT_EQ(f.alloc_run(4), arena_base() + 10u * granule) << "the fit below the hint";
    uintptr_t const a = f.alloc();
    ASSERT_NE(a, 0u);
    EXPECT_EQ(frame_of(a, granule), 250u) << "the hint was dragged below the run it took";
    EXPECT_EQ(frame_of(f.alloc(), granule), 50u) << "and the wrap pass takes the rest";
}

TEST(Frame, a_hint_at_the_end_of_the_range_names_no_frame)
{
    // alloc_run can leave the hint one past the last frame while a frame below is still free.
    // The byte rounding puts readable, zero, not-a-frame bits exactly there, so anything that
    // tests the bit the hint names has to bound it first.
    constexpr size_t granule = 1024;
    constexpr size_t count = 67; // not a byte multiple, so the rounding bits exist
    poison_free(count * granule);
    kickos::FrameAllocator f;
    ASSERT_TRUE(f.init(arena_base(), count * granule, granule));
    fill(f);
    release_span(f, 10u, 1u, granule);
    release_span(f, count - 3u, 3u, granule);
    ASSERT_EQ(frame_of(f.alloc_run(3), granule), count - 3u) << "the run at the top of the range";
    ASSERT_EQ(f.frames_free(), 1u);
    uintptr_t const a = f.alloc();
    ASSERT_NE(a, 0u);
    EXPECT_EQ(frame_of(a, granule), 10u) << "a frame past the last one was handed out";
    EXPECT_EQ(f.alloc(), 0u);
}

TEST(Frame, an_interleaved_sequence_hands_no_frame_out_twice)
{
    // A shadow of the bitmap kept beside the allocator, checked after every call: over a
    // long mixed sequence the allocator must never hand out a frame it already gave away,
    // never refuse a run the layout allows, and never return a run that is not the first one
    // long enough.
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
            poison_free(count * granule);
            kickos::FrameAllocator f;
            ASSERT_TRUE(f.init(arena_base(), count * granule, granule));
            size_t const reserved = count - f.frames_total();
            std::vector<bool> taken(count, false);
            for (size_t i = 0; i < reserved; i++)
            {
                taken[i] = true;
            }
            uint64_t seed = 0x9E3779B97F4A7C15ull;
            for (size_t step = 0; step < 8000u; step++)
            {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                size_t const pick = static_cast<size_t>(seed % 100u);
                size_t const arg = static_cast<size_t>((seed >> 8) % (count + 2u));
                if (pick < 45u)
                {
                    uintptr_t const a = f.alloc();
                    if (a == 0u)
                    {
                        continue;
                    }
                    size_t const i = frame_of(a, granule);
                    ASSERT_LT(i, count) << "step " << step;
                    ASSERT_GE(i, reserved) << "step " << step;
                    ASSERT_FALSE(taken[i]) << "frame " << i << " handed out twice, step " << step;
                    taken[i] = true;
                }
                else if (pick < 65u)
                {
                    size_t pages = 1u + static_cast<size_t>((seed >> 24) % 12u);
                    // The first run of `pages` free frames, which is the answer alloc_run owes.
                    size_t want = count;
                    size_t run = 0;
                    for (size_t i = reserved; i < count; i++)
                    {
                        if (taken[i])
                        {
                            run = 0;
                            continue;
                        }
                        run++;
                        if (run == pages)
                        {
                            want = i + 1u - pages;
                            break;
                        }
                    }
                    uintptr_t const a = f.alloc_run(pages);
                    if (want == count)
                    {
                        ASSERT_EQ(a, 0u) << "a run of " << pages << " was invented, step " << step;
                        continue;
                    }
                    ASSERT_NE(a, 0u) << "a run of " << pages << " fits at " << want
                                     << " and was refused, step " << step;
                    ASSERT_EQ(frame_of(a, granule), want) << "not the first fit, step " << step;
                    for (size_t k = 0; k < pages; k++)
                    {
                        taken[want + k] = true;
                    }
                }
                else
                {
                    bool expect = arg < count and arg >= reserved and taken[arg];
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
                }
                ASSERT_EQ(f.frames_free(), free_now) << "the free count drifted, step " << step;
            }
        }
    }
}

// 8 KiB is in the list on purpose: SPARC v9's smallest page is 8 KiB, which is F7's own
// reason for the granule being asked rather than believed.
INSTANTIATE_TEST_SUITE_P(Granules, FrameGranule,
                         ::testing::Values(size_t(1024), size_t(4096), size_t(8192),
                                           size_t(16384), size_t(65536)));
