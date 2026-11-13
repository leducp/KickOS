// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// CCR as the ARMv7-M bring-up leaves it, from every value a boot ROM can leave in the bits it
// owns. The bit positions are the ARMv7-M ARM's (DDI0403E B3.2.8), not the header's.

#include "arch/arm/armv7m/regs.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    constexpr uint32_t USERSETMPEND = 1u << 1;
    constexpr uint32_t UNALIGN_TRP = 1u << 3;
    constexpr uint32_t DIV_0_TRP = 1u << 4;

    TEST(CcrBringup, the_three_traps_are_set_as_bring_up_needs_whatever_the_rom_left)
    {
        // NONBASETHRDENA, USERSETMPEND, UNALIGN_TRP, DIV_0_TRP, BFHFNMIGN, STKALIGN, DC, IC, BP.
        uint32_t const bits[] = {1u << 0, 1u << 1, 1u << 3, 1u << 4, 1u << 8, 1u << 9,
                                 1u << 16, 1u << 17, 1u << 18};
        constexpr uint32_t COUNT = sizeof(bits) / sizeof(bits[0]);
        for (uint32_t pattern = 0; pattern < (1u << COUNT); pattern++)
        {
            uint32_t found = 0u;
            for (uint32_t i = 0; i < COUNT; i++)
            {
                if ((pattern & (1u << i)) != 0u)
                {
                    found |= bits[i];
                }
            }
            uint32_t const left = kickos::armv7m::ccr_at_bringup(found);
            EXPECT_EQ(left & USERSETMPEND, 0u) << std::hex << found;
            EXPECT_EQ(left & UNALIGN_TRP, 0u) << std::hex << found;
            EXPECT_EQ(left & DIV_0_TRP, DIV_0_TRP) << std::hex << found;
            uint32_t const owned = USERSETMPEND | UNALIGN_TRP | DIV_0_TRP;
            EXPECT_EQ(left & ~owned, found & ~owned) << std::hex << found;
        }
    }
}
