// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F's chip-latched fault address is the lowest port's error the core's bus master made,
// and every latch is cleared with the SYSMPU left enabled.

#include KICKOS_K64F_SYSMPU_ERROR_H

#include <gtest/gtest.h>

namespace
{
    uint32_t g_cesr = 0;
    uint32_t g_ear[SYSMPU_ERROR_PORTS] = {};
    uint32_t g_edr[SYSMPU_ERROR_PORTS] = {};
    bool g_vld_dropped = false;

    constexpr uint32_t CORE = 0u;
    constexpr uint32_t DEBUGGER = 1u;
    constexpr uint32_t DMA = 2u;

    void reset()
    {
        g_cesr = SYSMPU_ERROR_CESR_VLD;
        g_vld_dropped = false;
        for (size_t p = 0; p < SYSMPU_ERROR_PORTS; p++)
        {
            g_ear[p] = 0;
            g_edr[p] = 0;
        }
    }

    void latch(size_t port, uint32_t master, uint32_t ear)
    {
        g_cesr |= 1u << (31u - port);
        g_ear[port] = ear;
        g_edr[port] = master << 4;
    }

    uint32_t latched()
    {
        return g_cesr >> 27;
    }
}

extern "C"
{
    uint32_t sysmpu_error_read(uintptr_t offset)
    {
        if (offset == SYSMPU_ERROR_CESR)
        {
            return g_cesr;
        }
        size_t const port = (offset - 0x010u) / 8u;
        if ((offset - 0x010u) % 8u == 0u)
        {
            return g_ear[port];
        }
        return g_edr[port];
    }

    // SPERR is write-1-to-clear, VLD plain read/write (RM 19.3.1).
    void sysmpu_error_write(uintptr_t offset, uint32_t value)
    {
        ASSERT_EQ(offset, SYSMPU_ERROR_CESR);
        if ((value & SYSMPU_ERROR_CESR_VLD) == 0u)
        {
            g_vld_dropped = true;
        }
        g_cesr = (g_cesr & ~(value & 0xF8000000u) & ~SYSMPU_ERROR_CESR_VLD)
                 | (value & SYSMPU_ERROR_CESR_VLD);
    }
}

namespace
{
    TEST(K64fSysmpuError, NothingLatchedGivesNoAddress)
    {
        reset();
        uintptr_t addr = 0;
        EXPECT_FALSE(sysmpu_core_error_addr(&addr));
        EXPECT_EQ(g_cesr, SYSMPU_ERROR_CESR_VLD);
    }

    TEST(K64fSysmpuError, TheCoresErrorIsTakenOverALowerPortsDma)
    {
        reset();
        latch(0, DMA, 0x1000u);
        latch(3, CORE, 0x20000040u);
        uintptr_t addr = 0;
        ASSERT_TRUE(sysmpu_core_error_addr(&addr));
        EXPECT_EQ(addr, 0x20000040u);
        EXPECT_EQ(latched(), 0u) << "a latch left standing labels the next fault";
        EXPECT_FALSE(g_vld_dropped);
    }

    TEST(K64fSysmpuError, AnotherMastersErrorAloneExplainsNothing)
    {
        reset();
        latch(1, DMA, 0x1fff0000u);
        latch(4, DEBUGGER, 0x60000000u);
        uintptr_t addr = 0;
        EXPECT_FALSE(sysmpu_core_error_addr(&addr));
        EXPECT_EQ(latched(), 0u);
        EXPECT_FALSE(g_vld_dropped);
    }

    TEST(K64fSysmpuError, TwoCoreErrorsGiveTheLowerPorts)
    {
        reset();
        latch(4, CORE, 0x60000010u);
        latch(1, CORE, 0x1fff0100u);
        uintptr_t addr = 0;
        ASSERT_TRUE(sysmpu_core_error_addr(&addr));
        EXPECT_EQ(addr, 0x1fff0100u);
        EXPECT_EQ(latched(), 0u);
    }
}
