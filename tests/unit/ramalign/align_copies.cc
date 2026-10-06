// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// arch_ram_region_align, cmake/boot_arena.cmake's kickos_region_align and the composition tool's
// ram_align answer every row of one table alike.

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
}
