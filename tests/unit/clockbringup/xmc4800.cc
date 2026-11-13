// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The XMC4800's clock bring-up with each of its waits timing out in turn. The fCPU it returns
// and the fPERIPH the USIC is priced at must be the ones SYSCLKCR, PLLCON1, CPUCLKCR and PBCLKCR
// select (RM V1.3 ch.11), and the console must find a 115200 divisor at that fPERIPH.

#include "arch/arm/chip/xmc4800/clock_bringup.h"
#include "arch/arm/chip/xmc4800/regs/usic.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace scu = kickos::xmc::reg::scu;
    namespace ct = kickos::xmc::clocktree;

    constexpr uint32_t PLLCON0_RESET =
        scu::PLLCON0_VCOBYP | scu::PLLCON0_VCOPWD | scu::PLLCON0_PLLPWD;
    constexpr uint32_t OSCHPCTRL_RESET = scu::OSCHPCTRL_MODE_MASK;

    struct Hardware
    {
        uint32_t fcpu_hz;
        uint32_t fperiph_hz;
    };

    // fSYS: fOFI, or fPLL = 12 MHz x (NDIV+1) / ((PDIV+1) x (K2DIV+1)) in normal mode.
    Hardware hardware()
    {
        uint32_t fsys = ct::FOFI_HZ;
        if ((rf::value(scu::SYSCLKCR) & scu::SYSCLKCR_SYSSEL_PLL) != 0u)
        {
            fsys = 0u;
            if ((rf::value(scu::PLLCON0) & scu::PLLCON0_VCOBYP) == 0u)
            {
                uint32_t const con1 = rf::value(scu::PLLCON1);
                uint32_t const ndiv = ((con1 >> 8) & 0x7Fu) + 1u;
                uint32_t const k2div = ((con1 >> 16) & 0x7Fu) + 1u;
                uint32_t const pdiv = ((con1 >> 24) & 0xFu) + 1u;
                fsys = 12000000u * ndiv / (pdiv * k2div);
            }
        }
        uint32_t const fcpu = fsys / ((rf::value(scu::CPUCLKCR) & 1u) + 1u);
        return {fcpu, fcpu / ((rf::value(scu::PBCLKCR) & 1u) + 1u)};
    }

    void power_on()
    {
        rf::reset();
        rf::seed(scu::PLLCON0, PLLCON0_RESET);
        rf::seed(scu::OSCHPCTRL, OSCHPCTRL_RESET);
    }

    TEST(ClockBringupXmc4800, every_wait_answered_lands_on_the_pll)
    {
        power_on();
        uint32_t const fcpu = ct::bring_up();
        Hardware const hw = hardware();
        EXPECT_EQ(fcpu, ct::FCPU_PLL_HZ);
        EXPECT_EQ(hw.fcpu_hz, fcpu);
        EXPECT_EQ(hw.fperiph_hz, ct::fperiph_hz(fcpu));
        EXPECT_EQ(hw.fperiph_hz, 72000000u);
        kickos::xmc::reg::usic::Baud b = {};
        EXPECT_TRUE(kickos::xmc::reg::usic::baud_115200_for(hw.fperiph_hz, &b));
    }

    TEST(ClockBringupXmc4800, a_wait_that_times_out_stays_on_fofi)
    {
        struct Case
        {
            uint32_t mask;
            char const* what;
        };
        Case const cases[] = {
            {scu::PLLSTAT_OSC_USABLE, "the crystal is never usable"},
            {scu::PLLSTAT_VCOLOCK, "the PLL never locks"},
        };
        for (Case const& c : cases)
        {
            power_on();
            rf::plant_timeout(scu::PLLSTAT, c.mask);
            uint32_t const fcpu = ct::bring_up();
            Hardware const hw = hardware();
            EXPECT_EQ(rf::timeouts(), 1u) << c.what;
            EXPECT_EQ(fcpu, ct::FOFI_HZ) << c.what;
            EXPECT_EQ(hw.fcpu_hz, fcpu) << c.what;
            EXPECT_EQ(ct::fperiph_hz(fcpu), hw.fperiph_hz) << c.what;
            EXPECT_EQ(hw.fperiph_hz, ct::FOFI_HZ) << c.what;
            kickos::xmc::reg::usic::Baud b = {};
            EXPECT_TRUE(kickos::xmc::reg::usic::baud_115200_for(hw.fperiph_hz, &b)) << c.what;
        }
    }
}
