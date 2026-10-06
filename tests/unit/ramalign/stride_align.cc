// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include <stddef.h>

namespace
{
    size_t g_min_region = 0;
    int g_pow2 = 0;

    constexpr size_t STRIDE = KICKOS_TLS_STRIDE;
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
}
