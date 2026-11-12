// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The MK64F's clock bring-up with each of its waits timing out in turn. The rate it returns must
// be the MCGOUTCLK that MCG_C1, C5 and C6 select (K64 RM 25.4), and the console divisor the one
// UART0 needs at that rate.

#include "arch/arm/chip/mk64f/clock_bringup.h"
#include "arch/arm/chip/mk64f/regs/uart.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace mcg = kickos::mk64f::reg::mcg;
    namespace ct = kickos::mk64f::clocktree;

    constexpr uint32_t CLKDIV1 = 0x01140000u;
    constexpr uint32_t EXT_HZ = 50000000u;
    constexpr uint32_t IRC_SLOW_HZ = 32768u;

    // MCGOUTCLK: CLKS selects the FLL/PLL, the internal or the external reference; PLLS the PLL
    // over the FLL; IREFS the FLL's reference, x640 in the reset DCO range.
    uint32_t mcgoutclk()
    {
        uint32_t const c1 = rf::value(mcg::C1) & 0xFFu;
        uint32_t const c5 = rf::value(mcg::C5) & 0xFFu;
        uint32_t const c6 = rf::value(mcg::C6) & 0xFFu;
        uint32_t const clks = c1 >> 6;
        if (clks == 1u)
        {
            return IRC_SLOW_HZ;
        }
        if (clks == 2u)
        {
            return EXT_HZ;
        }
        if (clks != 0u)
        {
            return 0u;
        }
        if ((c6 & mcg::C6_PLLS) != 0u)
        {
            return EXT_HZ / ((c5 & 0x1Fu) + 1u) * ((c6 & 0x1Fu) + 24u);
        }
        if ((c1 & mcg::C1_IREFS_INT) != 0u)
        {
            return IRC_SLOW_HZ * 640u;
        }
        uint32_t const frdiv = (c1 >> 3) & 0x7u;
        uint32_t const divs[] = {32u, 64u, 128u, 256u, 512u, 1024u, 1280u, 1536u};
        return EXT_HZ / divs[frdiv] * 640u;
    }

    TEST(ClockBringupMk64f, every_wait_answered_lands_on_pee)
    {
        rf::reset();
        EXPECT_EQ(ct::bring_up(CLKDIV1), ct::PEE_HZ);
        EXPECT_EQ(mcgoutclk(), ct::PEE_HZ);
        EXPECT_EQ(rf::value(kickos::mk64f::reg::sim::CLKDIV1), CLKDIV1);
        EXPECT_EQ(kickos::mk64f::reg::uart::baud_sbr(ct::PEE_HZ, 115200u), 65u);
    }

    TEST(ClockBringupMk64f, a_wait_that_times_out_lands_back_on_fei)
    {
        struct Case
        {
            uint8_t mask;
            uint8_t want;
            char const* what;
        };
        Case const cases[] = {
            {mcg::S_IREFST, 0u, "the external reference is never selected"},
            {mcg::S_CLKST_MASK, mcg::S_CLKST_EXT, "MCGOUTCLK never reaches the external clock"},
            {mcg::S_PLLST, mcg::S_PLLST, "the PLL is never the PLLS source"},
            {mcg::S_LOCK0, mcg::S_LOCK0, "the PLL never locks"},
            {mcg::S_CLKST_MASK, mcg::S_CLKST_PLL, "MCGOUTCLK never reaches the PLL"},
        };
        for (Case const& c : cases)
        {
            rf::reset();
            rf::plant_timeout(mcg::S, c.mask, c.want);
            EXPECT_EQ(ct::bring_up(CLKDIV1), ct::FEI_HZ) << c.what;
            EXPECT_EQ(rf::timeouts(), 1u) << c.what;
            EXPECT_EQ(mcgoutclk(), ct::FEI_HZ) << c.what;
            EXPECT_EQ(kickos::mk64f::reg::uart::baud_sbr(ct::FEI_HZ, 115200u), 11u) << c.what;
            EXPECT_EQ(kickos::mk64f::reg::uart::baud_brfa(ct::FEI_HZ, 115200u), 12u) << c.what;
        }
    }
}
