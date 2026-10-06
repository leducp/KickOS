// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The v7-M reporter reads the frame unless the CFSR names an aborted stacking or unstacking,
// on whatever stack it sits. A chip-latched address labels only a bus fault the core holds no
// address for.

#include KICKOS_V7M_FAULT_FRAME_H

#include <gtest/gtest.h>

namespace
{
    constexpr uint32_t MUNSTKERR = 1u << 3;
    constexpr uint32_t MSTKERR = 1u << 4;
    constexpr uint32_t MLSPERR = 1u << 5;
    constexpr uint32_t UNSTKERR = 1u << 11;
    constexpr uint32_t STKERR = 1u << 12;
    constexpr uint32_t LSPERR = 1u << 13;

    TEST(V7mFaultFrame, AStackingOrUnstackingAbortLeavesTheFrameUnread)
    {
        for (uint32_t bit : {MUNSTKERR, MSTKERR, UNSTKERR, STKERR})
        {
            EXPECT_FALSE(armv7m_fault_frame_readable(bit)) << bit;
            EXPECT_FALSE(armv7m_fault_frame_readable(bit | (1u << 1) | (1u << 16))) << bit;
        }
    }

    TEST(V7mFaultFrame, EveryOtherFaultReadsTheFrame)
    {
        EXPECT_TRUE(armv7m_fault_frame_readable(0u));
        EXPECT_TRUE(armv7m_fault_frame_readable(MLSPERR | LSPERR));
        uint32_t const others = ~(MUNSTKERR | MSTKERR | UNSTKERR | STKERR);
        for (uint32_t bit = 1u; bit != 0u; bit <<= 1)
        {
            if ((bit & others) != 0u)
            {
                EXPECT_TRUE(armv7m_fault_frame_readable(bit)) << bit;
            }
        }
    }

    constexpr uint32_t IACCVIOL = 1u << 0;
    constexpr uint32_t IBUSERR = 1u << 8;
    constexpr uint32_t PRECISERR = 1u << 9;
    constexpr uint32_t IMPRECISERR = 1u << 10;
    constexpr uint32_t BFARVALID = 1u << 15;
    constexpr uint32_t UNDEFINSTR = 1u << 16;
    constexpr uint32_t UNALIGNED = 1u << 24;
    constexpr uint32_t DIVBYZERO = 1u << 25;

    TEST(V7mFaultFrame, AChipAddressExplainsABusFaultWithNoCoreAddress)
    {
        EXPECT_TRUE(armv7m_chip_addr_explains(IMPRECISERR));
        EXPECT_TRUE(armv7m_chip_addr_explains(PRECISERR));
        EXPECT_FALSE(armv7m_chip_addr_explains(PRECISERR | BFARVALID));
        EXPECT_FALSE(armv7m_chip_addr_explains(IBUSERR));
        EXPECT_FALSE(armv7m_chip_addr_explains(0u));
    }

    TEST(V7mFaultFrame, AUsageOrMemManageFaultNeverTakesAChipAddress)
    {
        for (uint32_t usage : {UNDEFINSTR, UNALIGNED, DIVBYZERO})
        {
            EXPECT_FALSE(armv7m_chip_addr_explains(usage)) << usage;
            EXPECT_FALSE(armv7m_chip_addr_explains(usage | IMPRECISERR)) << usage;
        }
        EXPECT_FALSE(armv7m_chip_addr_explains(IACCVIOL | IMPRECISERR));
    }
}
