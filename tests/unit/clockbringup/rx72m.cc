// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RX72M's clock bring-up with each oscillator it polls never stabilising, in turn. The ICLK
// and PCLKB it reads back must be the ones SCKCR3, PLLCR, HOCOCR2 and SCKCR select (UM sec.9.2),
// and the console divisor the one SCI6 can run at that PCLKB.

#include "arch/rx/chip/rx72m/clock_bringup.h"
#include "arch/rx/chip/rx72m/regs/sci.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace cgc = kickos::rx::reg::cgc;
    namespace ct = kickos::rx::clocktree;
    namespace sci = kickos::rx::reg::sci;

    constexpr uint8_t OVF_ALL = cgc::OSCOVFSR_MOOVF | cgc::OSCOVFSR_PLOVF | cgc::OSCOVFSR_HCOVF;

    struct Hardware
    {
        uint32_t iclk_hz;
        uint32_t pclkb_hz;
    };

    // The source CKSEL picks: LOCO 240 kHz, HOCO 16/18/20 MHz, the 24 MHz crystal, or the PLL at
    // its input x (STC+1)/2 / (PLIDIV+1). ICK and PCKB divide it by 2^n.
    Hardware hardware()
    {
        uint32_t const cksel = (rf::value(cgc::SCKCR3) >> 8) & 0x7u;
        uint32_t const hcfrq = rf::value(cgc::HOCOCR2) & 0x3u;
        uint32_t const hoco[] = {16000000u, 18000000u, 20000000u, 0u};
        uint32_t src = 0u;
        if (cksel == 0u)
        {
            src = 240000u;
        }
        else if (cksel == 1u)
        {
            src = hoco[hcfrq];
        }
        else if (cksel == 2u)
        {
            src = 24000000u;
        }
        else if (cksel == 4u)
        {
            uint32_t const pllcr = rf::value(cgc::PLLCR) & 0xFFFFu;
            uint32_t in = 24000000u;
            if ((pllcr & 0x10u) != 0u)
            {
                in = hoco[hcfrq];
            }
            src = static_cast<uint32_t>(static_cast<uint64_t>(in) * (((pllcr >> 8) & 0x3Fu) + 1u)
                                        / (2u * ((pllcr & 0x3u) + 1u)));
        }
        uint32_t const sckcr = rf::value(cgc::SCKCR);
        return {src >> ((sckcr >> 24) & 0xFu), src >> ((sckcr >> 8) & 0xFu)};
    }

    void power_on(uint8_t ovf)
    {
        rf::reset();
        rf::seed(cgc::OSCOVFSR, ovf);
        rf::seed(cgc::HOCOCR, cgc::HOCOCR_HCSTP);
    }

    TEST(ClockBringupRx72m, every_oscillator_stable_lands_on_the_pll)
    {
        power_on(OVF_ALL);
        ct::bring_up();
        Hardware const hw = hardware();
        EXPECT_EQ(hw.iclk_hz, 240000000u);
        EXPECT_EQ(hw.pclkb_hz, 60000000u);
        EXPECT_EQ(ct::iclk_hz(), hw.iclk_hz);
        EXPECT_EQ(ct::pclkb_hz(), hw.pclkb_hz);
        sci::BaudSetting bs = {};
        EXPECT_TRUE(sci::baud_select(ct::pclkb_hz(), 115200u, &bs));
    }

    TEST(ClockBringupRx72m, an_oscillator_that_never_stabilises_lands_on_the_hoco)
    {
        struct Case
        {
            uint8_t never;
            char const* what;
        };
        Case const cases[] = {
            {cgc::OSCOVFSR_MOOVF, "the crystal never stabilises"},
            {cgc::OSCOVFSR_PLOVF, "the PLL never stabilises"},
        };
        for (Case const& c : cases)
        {
            power_on(OVF_ALL & ~c.never);
            ct::bring_up();
            Hardware const hw = hardware();
            EXPECT_EQ(hw.pclkb_hz, cgc::HOCO_20MHZ) << c.what;
            EXPECT_EQ(ct::iclk_hz(), hw.iclk_hz) << c.what;
            EXPECT_EQ(ct::pclkb_hz(), hw.pclkb_hz) << c.what;
            sci::BaudSetting bs = {};
            ASSERT_TRUE(sci::baud_select(ct::pclkb_hz(), 115200u, &bs)) << c.what;
            EXPECT_EQ(bs.brr, 10u) << c.what;
        }
    }

    TEST(ClockBringupRx72m, no_oscillator_stable_stays_on_the_loco_where_no_baud_lands)
    {
        power_on(cgc::OSCOVFSR_PLOVF);
        ct::bring_up();
        Hardware const hw = hardware();
        EXPECT_EQ(hw.pclkb_hz, 240000u);
        EXPECT_EQ(ct::pclkb_hz(), hw.pclkb_hz);
        sci::BaudSetting bs = {};
        EXPECT_FALSE(sci::baud_select(ct::pclkb_hz(), 115200u, &bs));
    }
}
