// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The STM32F411's clock bring-up with each of its waits timing out in turn. The rates it returns
// must be the ones RCC_CFGR runs the part at (RM0383 sec.6.3.3), and the console divisor is the
// one USART2 needs at that APB1 rate.

#include "arch/arm/chip/stm32f411/clock_bringup.h"
#include "arch/arm/common/stm32_usart.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace rcc = kickos::stm32f411::reg::rcc;
    namespace ct = kickos::stm32f411::clocktree;

    constexpr uint32_t HSE_HZ = 8000000u;
    constexpr uint32_t PLLCFGR = 0x07405408u;

    // SYSCLK from CFGR.SWS, then HPRE and PPRE1 (RM0383 sec.6.3.3).
    struct Hardware
    {
        uint32_t sysclk_hz;
        uint32_t pclk1_hz;
    };

    Hardware from_cfgr(uint32_t cfgr)
    {
        uint32_t sysclk = 0u;
        uint32_t const sws = (cfgr >> 2) & 0x3u;
        if (sws == 0u)
        {
            sysclk = ct::HSI_HZ;
        }
        else if (sws == 1u)
        {
            sysclk = HSE_HZ;
        }
        else if (sws == 2u)
        {
            sysclk = ct::SYSCLK_PLL_HZ;
        }
        uint32_t const hpre = (cfgr >> 4) & 0xFu;
        uint32_t hclk = sysclk;
        if ((hpre & 0x8u) != 0u)
        {
            uint32_t const shifts[] = {1u, 2u, 3u, 4u, 6u, 7u, 8u, 9u};
            hclk = sysclk >> shifts[hpre & 0x7u];
        }
        uint32_t const ppre1 = (cfgr >> 10) & 0x7u;
        uint32_t pclk1 = hclk;
        if ((ppre1 & 0x4u) != 0u)
        {
            pclk1 = hclk >> ((ppre1 & 0x3u) + 1u);
        }
        return {sysclk, pclk1};
    }

    struct Case
    {
        uintptr_t reg;
        uint32_t mask;
        char const* what;
    };

    TEST(ClockBringupStm32f411, every_wait_answered_lands_on_the_pll)
    {
        rf::reset();
        ct::Landed const l = ct::bring_up(PLLCFGR);
        Hardware const hw = from_cfgr(rf::value(rcc::CFGR));
        EXPECT_EQ(l.sysclk_hz, ct::SYSCLK_PLL_HZ);
        EXPECT_EQ(l.pclk1_hz, ct::PCLK1_PLL_HZ);
        EXPECT_EQ(hw.sysclk_hz, l.sysclk_hz);
        EXPECT_EQ(hw.pclk1_hz, l.pclk1_hz);
        EXPECT_EQ(rf::value(rcc::PLLCFGR), PLLCFGR);
        EXPECT_EQ(kickos::stm32::usart_brr(l.pclk1_hz, 115200u), 365u);
    }

    TEST(ClockBringupStm32f411, a_wait_that_times_out_stays_on_the_hsi)
    {
        Case const cases[] = {
            {rcc::CR, rcc::CR_HSERDY, "the crystal never starts"},
            {rcc::CR, rcc::CR_PLLRDY, "the PLL never locks"},
            {rcc::CFGR, rcc::CFGR_SWS_PLL, "the switch to the PLL is never confirmed"},
        };
        for (Case const& c : cases)
        {
            rf::reset();
            rf::plant_timeout(c.reg, c.mask);
            ct::Landed const l = ct::bring_up(PLLCFGR);
            Hardware const hw = from_cfgr(rf::value(rcc::CFGR));
            EXPECT_EQ(rf::timeouts(), 1u) << c.what;
            EXPECT_EQ(l.sysclk_hz, ct::HSI_HZ) << c.what;
            EXPECT_EQ(l.pclk1_hz, ct::HSI_HZ) << c.what;
            EXPECT_EQ(hw.sysclk_hz, l.sysclk_hz) << c.what;
            EXPECT_EQ(hw.pclk1_hz, l.pclk1_hz) << c.what;
            EXPECT_EQ(kickos::stm32::usart_brr(l.pclk1_hz, 115200u), 139u) << c.what;
        }
    }
}
