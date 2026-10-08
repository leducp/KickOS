// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// arch_ram_region_size and arch_ram_region_align, and the composition tool's ram_size and
// ram_align, answer every row of one table alike, and the tool refuses exactly the rows C cannot
// round. At a stride, where the thread pointer is SP masked down to it, the stride leg is also
// checked on each region geometry by value.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include <climits>

#include "align_table.h"

static_assert(sizeof(size_t) * CHAR_BIT == REGION_WORD_BITS,
              "the table's figures are the tool's for a word of another width");

namespace
{
    size_t g_min_region = 0;
    int g_pow2 = 0;

#if defined(KICKOS_TLS) && KICKOS_TLS && KICKOS_TLS_FROM_SP
    constexpr size_t STRIDE = KICKOS_TLS_STRIDE;
#else
    constexpr size_t STRIDE = 0;
#endif
}

extern "C"
{
    size_t arch_mpu_min_region(void)
    {
        return g_min_region;
    }

    int arch_mpu_region_pow2(void)
    {
        return g_pow2;
    }
}

namespace
{
    // Whether `size` is a region the rule could answer for `want`: at least it, on the unit, and a
    // power of two where the unit asks one.
    bool rounds(RegionCase const& c, size_t size)
    {
        size_t unit = c.min_region;
        if (unit == 0)
        {
            unit = 16u;
        }
        if (size < c.want or size % unit != 0)
        {
            return false;
        }
        return c.min_region == 0 or c.pow2 == 0 or (size & (size - 1u)) == 0;
    }

    TEST(RamAlignCopies, TheTwoCopiesAgreeOnEveryRow)
    {
        size_t rows = 0;
        for (RegionCase const& c : REGION_CASES)
        {
            if (c.stride != STRIDE)
            {
                continue;
            }
            rows++;
            g_min_region = c.min_region;
            g_pow2 = c.pow2;
            if (c.refused)
            {
                EXPECT_FALSE(rounds(c, arch_ram_region_size(c.want)))
                    << "refused by the tool, rounded by C: min " << c.min_region << " pow2 "
                    << c.pow2 << " want " << c.want;
                continue;
            }
            EXPECT_TRUE(rounds(c, arch_ram_region_size(c.want)))
                << "min " << c.min_region << " pow2 " << c.pow2 << " want " << c.want;
            EXPECT_EQ(c.size, arch_ram_region_size(c.want))
                << "size: min " << c.min_region << " pow2 " << c.pow2 << " want " << c.want;
            EXPECT_EQ(c.align, arch_ram_region_align(c.want))
                << "align: min " << c.min_region << " pow2 " << c.pow2 << " stride " << c.stride
                << " want " << c.want;
        }
        EXPECT_NE(rows, 0u) << "the table holds no row at this build's stride";
    }

#if defined(KICKOS_TLS) && KICKOS_TLS && KICKOS_TLS_FROM_SP
    void geometry(size_t min_region, int pow2)
    {
        g_min_region = min_region;
        g_pow2 = pow2;
    }

    TEST(RamAlign, ABaseLimitMpuStridesOnlyAStackSizedBlock)
    {
        geometry(32u, 0);
        EXPECT_EQ(arch_ram_region_align(STRIDE), STRIDE);
        EXPECT_EQ(arch_ram_region_align(STRIDE - 20u), STRIDE);
        EXPECT_EQ(arch_ram_region_align(STRIDE / 2u), 32u);
        EXPECT_EQ(arch_ram_region_align(STRIDE - 64u), 32u);
        EXPECT_EQ(arch_ram_region_align(STRIDE + 32u), 32u);
        EXPECT_EQ(arch_ram_region_align(96u), 32u);
    }

    TEST(RamAlign, NoMpuStridesOnlyAStackSizedBlock)
    {
        geometry(0u, 1);
        EXPECT_EQ(arch_ram_region_align(STRIDE), STRIDE);
        EXPECT_EQ(arch_ram_region_align(STRIDE / 2u), 16u);
        EXPECT_EQ(arch_ram_region_align(2u * STRIDE), 16u);
    }

    TEST(RamAlign, APow2MpuAlignsEveryBlockToItsSize)
    {
        geometry(32u, 1);
        EXPECT_EQ(arch_ram_region_align(STRIDE), STRIDE);
        EXPECT_EQ(arch_ram_region_align(STRIDE / 2u), STRIDE / 2u);
        EXPECT_EQ(arch_ram_region_align(2u * STRIDE), 2u * STRIDE);
        EXPECT_EQ(arch_ram_region_align(100u), 128u);
    }
#endif
}
