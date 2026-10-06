// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread's SYSMPU descriptor grants the core's user mode its region's rights and no other
// master anything: logical master 1 is the debugger (K64 RM 3.3.7.2, Table 3-20).

#include KICKOS_K64F_SYSMPU_RIGHTS_H

#include <gtest/gtest.h>

namespace
{
    constexpr uint32_t M0UM = 0x7u;

    TEST(K64fSysmpuRights, TheCoreUserModeGetsTheRegionsRights)
    {
        EXPECT_EQ(sysmpu_word2(ARCH_MPU_R), SYSMPU_WORD2_M0UM_R);
        EXPECT_EQ(sysmpu_word2(ARCH_MPU_R | ARCH_MPU_W),
                  SYSMPU_WORD2_M0UM_R | SYSMPU_WORD2_M0UM_W);
        EXPECT_EQ(sysmpu_word2(ARCH_MPU_R | ARCH_MPU_X),
                  SYSMPU_WORD2_M0UM_R | SYSMPU_WORD2_M0UM_X);
    }

    TEST(K64fSysmpuRights, NoOtherFieldIsEverGranted)
    {
        uint32_t const all =
            ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_X | ARCH_MPU_DEV | ARCH_MPU_NOCACHE;
        for (uint32_t attr = 0; attr <= all; attr++)
        {
            EXPECT_EQ(sysmpu_word2(attr) & ~M0UM, 0u) << "attr " << attr;
        }
    }
}
