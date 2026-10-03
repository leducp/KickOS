// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The clear kos_ram_alloc gives a region board's block, over the REAL kernel/mem/ramown.cc
// compiled with KICKOS_ARCH_ARENA_DCACHE set, the posture of an arch that puts a data cache
// over the arena. The maintenance is recorded here in place of the arch's: a clean alone
// leaves the block's lines valid in the cache, so it must be the clean-and-invalidate, issued
// over the zeroed extent after the zeroes are written.

#include <kickos/ramown.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if not KICKOS_ARCH_ARENA_DCACHE
#error "this gate compiles ramown.cc with a data cache over the arena"
#endif

namespace
{
    unsigned char g_block[256];

    int g_cleans = 0;
    int g_clean_invalidates = 0;
    void const* g_at = nullptr;
    size_t g_bytes = 0;
    bool g_zeroed_first = false;

    bool block_zero()
    {
        for (size_t i = 0; i < sizeof(g_block); i++)
        {
            if (g_block[i] != 0)
            {
                return false;
            }
        }
        return true;
    }
}

extern "C"
{
    void arch_dcache_flush(void const* addr, size_t bytes)
    {
        g_cleans++;
        g_at = addr;
        g_bytes = bytes;
    }

    void arch_dcache_invalidate(void* addr, size_t bytes)
    {
        g_clean_invalidates++;
        g_at = addr;
        g_bytes = bytes;
        g_zeroed_first = block_zero();
    }
}

TEST(RamBlockClear, the_zeroes_are_cleaned_and_the_lines_dropped)
{
    memset(g_block, 0xA5, sizeof(g_block));

    kickos::ram_block_clear(g_block, sizeof(g_block));

    EXPECT_TRUE(block_zero());
    EXPECT_EQ(g_clean_invalidates, 1) << "one clean-and-invalidate over the block";
    EXPECT_EQ(g_cleans, 0) << "a clean alone leaves the lines valid in the cache";
    EXPECT_EQ(g_at, static_cast<void const*>(g_block));
    EXPECT_EQ(g_bytes, sizeof(g_block));
    EXPECT_TRUE(g_zeroed_first) << "the maintenance follows the zeroes";
}
