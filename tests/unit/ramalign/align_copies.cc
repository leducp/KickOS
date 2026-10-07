// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// arch_ram_region_align, cmake/boot_arena.cmake's kickos_region_align and the composition tool's
// ram_align answer every row of one table alike. At a stride, where the thread pointer is SP
// masked down to it, the stride leg is also checked on each region geometry by value.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include "align_table.h"

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
    TEST(RamAlignCopies, TheThreeCopiesAgreeOnEveryRow)
    {
        size_t rows = 0;
        for (AlignCase const& c : ALIGN_CASES)
        {
            if (c.stride != STRIDE)
            {
                continue;
            }
            rows++;
            g_min_region = c.min_region;
            g_pow2 = c.pow2;
            size_t const native = arch_ram_region_align(c.want);
            EXPECT_EQ(c.cmake, native) << "boot_arena.cmake: min " << c.min_region << " pow2 "
                                       << c.pow2 << " stride " << c.stride << " want " << c.want;
            EXPECT_EQ(c.python, native) << "supply.py: min " << c.min_region << " pow2 " << c.pow2
                                        << " stride " << c.stride << " want " << c.want;
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
