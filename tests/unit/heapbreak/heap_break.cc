// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// _sbrk over a 4000 B window: it grows to the window's end and no further, and never moves back;
// and kickos_heap_seat, at the image's first allocation, takes the whole window and gives it back,
// once, under newlib's full malloc at either word size; and it arms the exit's release of libc
// scratch before its first ask.

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include <vector>

asm(".pushsection .data\n"
    ".balign 16\n"
    ".globl _kickos_heap_start\n"
    "_kickos_heap_start:\n"
    ".zero 4000\n"
    ".globl _kickos_heap_limit\n"
    "_kickos_heap_limit:\n"
    ".popsection\n");

extern "C"
{
    extern char _kickos_heap_start[];
    extern char _kickos_heap_limit[];
    void* _sbrk(intptr_t incr);
    void kickos_heap_seat(void);
    void kos_ut_heap_reset(void);
    void* kos_ut_malloc(size_t n);
    void kos_ut_free(void* block);
}

namespace
{
    void* const FAILED = reinterpret_cast<void*>(-1);

    struct Ask
    {
        size_t n;
        bool taken;
    };

    // newlib's word size: its chunk header, its alignment twice that, its smallest chunk four.
    size_t g_word = 4;
    std::vector<Ask> g_asked;
    std::vector<void*> g_freed;
    // g_asked's size at each kickos_reent_heap_used.
    std::vector<size_t> g_armed;

    // newlib's full malloc on its first request: one _sbrk of the request's chunk and the
    // smallest chunk it keeps after it. The arms make no later request.
    size_t first_footprint(size_t n)
    {
        size_t const mask = 2 * g_word - 1;
        size_t const least = 4 * g_word;
        size_t chunk = (n + g_word + mask) & ~mask;
        if (chunk < least)
        {
            chunk = least;
        }
        return chunk + least;
    }

    char* brk_now()
    {
        return static_cast<char*>(_sbrk(0));
    }

    size_t window()
    {
        return static_cast<size_t>(_kickos_heap_limit - _kickos_heap_start);
    }

    class HeapBreak : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            kos_ut_heap_reset();
            g_word = 4;
            g_asked.clear();
            g_freed.clear();
            g_armed.clear();
        }

        // The seat's asks run down from the whole window, each refused until one fits; that one
        // ends the break at the window's end and is freed, and a second seat asks nothing.
        static void seat_takes_the_whole_window(size_t word)
        {
            g_word = word;
            ASSERT_EQ(brk_now(), _kickos_heap_start);
            kickos_heap_seat();
            size_t const asked = g_asked.size();
            kickos_heap_seat();
            EXPECT_EQ(g_asked.size(), asked);
            ASSERT_FALSE(g_asked.empty());
            EXPECT_EQ(g_asked.front().n, window());
            for (size_t i = 0; i + 1 < g_asked.size(); i++)
            {
                EXPECT_FALSE(g_asked[i].taken) << "ask " << i << " of " << g_asked[i].n;
                EXPECT_GT(g_asked[i].n, g_asked[i + 1].n);
            }
            Ask const last = g_asked.back();
            EXPECT_TRUE(last.taken);
            EXPECT_EQ(first_footprint(last.n), window());
            EXPECT_EQ(brk_now(), _kickos_heap_limit);
            ASSERT_EQ(g_freed.size(), 1u);
            EXPECT_EQ(g_freed.front(), static_cast<void*>(_kickos_heap_start));
        }
    };
}

extern "C" void* kos_ut_malloc(size_t n)
{
    void* const block = _sbrk(static_cast<intptr_t>(first_footprint(n)));
    g_asked.push_back({n, block != FAILED});
    if (block == FAILED)
    {
        return nullptr;
    }
    return block;
}

extern "C" void kos_ut_free(void* block)
{
    g_freed.push_back(block);
}

extern "C" void kickos_reent_heap_used(void)
{
    g_armed.push_back(g_asked.size());
}

TEST_F(HeapBreak, the_seat_arms_the_exit_release_before_its_first_ask)
{
    kickos_heap_seat();
    ASSERT_FALSE(g_armed.empty());
    EXPECT_EQ(g_armed.front(), 0u);
    EXPECT_FALSE(g_asked.empty());
}

TEST_F(HeapBreak, the_seat_takes_the_whole_window_and_gives_it_back_once_on_a_32_bit_newlib)
{
    seat_takes_the_whole_window(4);
}

TEST_F(HeapBreak, the_seat_takes_the_whole_window_and_gives_it_back_once_on_a_64_bit_newlib)
{
    seat_takes_the_whole_window(8);
}

TEST_F(HeapBreak, a_break_never_moves_back)
{
    ASSERT_NE(_sbrk(16), FAILED);
    char* const before = brk_now();
    EXPECT_EQ(before, _kickos_heap_start + 16);
    EXPECT_EQ(_sbrk(-8), FAILED);
    EXPECT_EQ(brk_now(), before);
}

TEST_F(HeapBreak, a_break_grows_to_the_window_end_and_no_further)
{
    ASSERT_NE(_sbrk(16), FAILED);
    char* const before = brk_now();
    intptr_t const room = _kickos_heap_limit - before;
    EXPECT_EQ(_sbrk(room + 1), FAILED);
    EXPECT_EQ(_sbrk(room), static_cast<void*>(before));
    EXPECT_EQ(brk_now(), _kickos_heap_limit);
    EXPECT_EQ(_sbrk(1), FAILED);
    EXPECT_EQ(brk_now(), _kickos_heap_limit);
}
