// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The spawn's device-window admission (grant_device_window_admit, kernel/grant/grant.cc) over a
// region chip's own tables, which CMake includes ahead of this file with the chip's case list:
// the host build's own tables are the sim's.

#include <kickos/grant.h>
#include <kickos/sys/errno.h>

#include "region_seam.h"

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

RegionSeam g_region_seam = {.min_region = KICKOS_CHIP_REGION_GRANULE,
                            .pow2 = KICKOS_CHIP_REGION_POW2,
                            .reserved = kickos::chip::reserved_blocks,
                            .apertures = kickos::chip::window_apertures,
                            .bus_masters = kickos::chip::bus_master_apertures};

namespace
{
    TEST(GrantAperture, each_window_answers_its_errno)
    {
        for (ApertureCase const& c : CASES)
        {
            EXPECT_EQ(kickos::grant_device_window_admit(c.base, c.size), c.answer) << c.name;
        }
    }

#if defined(GRANT_CODE_MAP_BLOCKS)
    TEST(GrantAperture, each_code_map_block_answers_what_the_selftest_expects)
    {
        uintptr_t const blocks[] = {GRANT_CODE_MAP_BLOCKS};
        int const answers[] = {GRANT_CODE_MAP_ANSWERS};
        static_assert(sizeof(blocks) / sizeof(blocks[0]) == sizeof(answers) / sizeof(answers[0]));
        for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        {
            EXPECT_EQ(kickos::grant_device_window_admit(blocks[i], 0x1000u), answers[i])
                << std::hex << blocks[i];
        }
    }
#endif
}
