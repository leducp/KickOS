// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SAM3X8E's clock bring-up with each PMC status bit it polls never rising, in turn. The MCK
// it returns must be the one CKGR_MOR, CKGR_PLLAR and PMC_MCKR select (datasheet sec.28.15), and
// the console divisor the one the UART needs at that rate.

#include "arch/arm/chip/sam3x8e/clock_bringup.h"
#include "arch/arm/chip/sam3x8e/uart_baud.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace ct = kickos::sam3x8e::clocktree;

    constexpr uint32_t SR_MOSCXTS = 1u << 0;
    constexpr uint32_t SR_LOCKA = 1u << 1;
    constexpr uint32_t SR_MCKRDY = 1u << 3;
    constexpr uint32_t SR_MOSCSELS = 1u << 16;
    constexpr uint32_t SR_ALL = SR_MOSCXTS | SR_LOCKA | SR_MCKRDY | SR_MOSCSELS;
    constexpr uint32_t MCKR_RESET = 0x1u;

    // MAINCK from MOR.MOSCSEL; PLLA = MAINCK x (MULA+1) / DIVA; MCK = the CSS source over PRES.
    uint32_t mck()
    {
        uint32_t const mor = rf::value(ct::CKGR_MOR);
        uint32_t const pllar = rf::value(ct::CKGR_PLLAR);
        uint32_t const mckr = rf::value(ct::PMC_MCKR);
        uint32_t mainck = 4000000u;
        if ((mor & (1u << 24)) != 0u)
        {
            mainck = 12000000u;
        }
        uint32_t src = 0u;
        uint32_t const css = mckr & 0x3u;
        if (css == 1u)
        {
            src = mainck;
        }
        else if (css == 2u)
        {
            uint32_t const mula = (pllar >> 16) & 0x7FFu;
            uint32_t const diva = pllar & 0xFFu;
            if (mula != 0u and diva != 0u)
            {
                src = mainck * (mula + 1u) / diva;
            }
        }
        uint32_t const pres = (mckr >> 4) & 0x7u;
        if (pres == 7u)
        {
            return src / 3u;
        }
        return src >> pres;
    }

    void power_on()
    {
        rf::reset();
        rf::seed(ct::PMC_MCKR, MCKR_RESET);
    }

    TEST(ClockBringupSam3x8e, every_source_ready_lands_on_plla_halved)
    {
        power_on();
        rf::seed(ct::PMC_SR, SR_ALL);
        EXPECT_EQ(ct::bring_up(), 84000000u);
        EXPECT_EQ(mck(), 84000000u);
        EXPECT_EQ(kickos::sam3x8e::uart_brgr_cd(84000000u, 115200u), 46u);
    }

    TEST(ClockBringupSam3x8e, a_source_that_never_comes_up_stays_on_the_last_confirmed_clock)
    {
        struct Case
        {
            uint32_t never;
            uint32_t mck_hz;
            uint32_t cd;
            char const* what;
        };
        Case const cases[] = {
            {SR_MOSCXTS, 4000000u, 2u, "the crystal never starts: the fast RC"},
            {SR_MOSCSELS, 4000000u, 2u, "MAINCK never confirms the crystal: the fast RC"},
            {SR_MCKRDY, 12000000u, 7u, "MCK never confirms the crystal"},
            {SR_LOCKA, 12000000u, 7u, "PLLA never locks: the crystal"},
        };
        for (Case const& c : cases)
        {
            power_on();
            rf::seed(ct::PMC_SR, SR_ALL & ~c.never);
            uint32_t const landed = ct::bring_up();
            EXPECT_EQ(landed, c.mck_hz) << c.what;
            EXPECT_EQ(mck(), landed) << c.what;
            EXPECT_EQ(kickos::sam3x8e::uart_brgr_cd(landed, 115200u), c.cd) << c.what;
        }
    }
}
