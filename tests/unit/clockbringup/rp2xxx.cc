// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2xxx clock bring-up, built once per part against its own family_map.h and regs/, with
// each of its waits timing out in turn. clk_sys must be the PLL the dividers program or the
// fallback the part stays on, and the console's divisors the ones for the clock clk_peri runs.

#include "arch/arm/chip/rp2xxx/clock_bringup.h"
#include "regs/uart.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace reg = kickos::rp2xxx::reg;
    namespace ct = kickos::rp2xxx::clocktree;

    constexpr uint32_t XOSC_HZ = 12000000u;
    constexpr uint32_t BAUD = 115200u;

    // REFDIV, FBDIV and the two post-dividers (DS 2.18.2 / 8.6.2).
    uint32_t pll_sys_hz()
    {
        uint32_t const refdiv = rf::value(reg::pll::CS) & 0x3Fu;
        uint32_t const fbdiv = rf::value(reg::pll::FBDIV_INT) & 0xFFFu;
        uint32_t const prim = rf::value(reg::pll::PRIM);
        uint32_t const post = ((prim >> 16) & 0x7u) * ((prim >> 12) & 0x7u);
        if (refdiv == 0u or post == 0u)
        {
            return 0u;
        }
        return XOSC_HZ / refdiv * fbdiv / post;
    }

    void power_on()
    {
        rf::reset();
        rf::seed(reg::clocks::CLK_REF_CTRL + 0x4u, 1u << 16); // CLK_REF_DIV, read by the RP2350
    }

    TEST(ClockBringupRp2xxx, every_wait_answered_lands_on_pll_sys)
    {
        power_on();
        uint32_t const clk_sys = ct::bring_up();
        EXPECT_EQ(clk_sys, reg::clocks::CLK_SYS_HZ);
        EXPECT_EQ(pll_sys_hz(), clk_sys);
        EXPECT_EQ(rf::value(reg::clocks::CLK_SYS_CTRL),
                  reg::clocks::CLK_SYS_AUXSRC_PLL | reg::clocks::CLK_SYS_SRC_AUX);
        EXPECT_EQ(rf::value(reg::clocks::CLK_PERI_CTRL), reg::clocks::CLK_PERI_ENABLE_CLK_SYS);
        EXPECT_TRUE(ct::console_clk_on_pll());
        EXPECT_EQ(reg::uart::IBRD_PLL, kickos::rp2xxx::pl011_ibrd(clk_sys, BAUD));
        EXPECT_EQ(reg::uart::FBRD_PLL, kickos::rp2xxx::pl011_fbrd(clk_sys, BAUD));
    }

    TEST(ClockBringupRp2xxx, a_pll_that_never_locks_leaves_clk_sys_and_clk_peri_off_it)
    {
        power_on();
        rf::plant_timeout(reg::pll::CS, reg::pll::CS_LOCK);
        uint32_t const clk_sys = ct::bring_up();
        EXPECT_EQ(rf::timeouts(), 1u);
        EXPECT_EQ(clk_sys, reg::clocks::CLK_SYS_NO_PLL_HZ);
        EXPECT_FALSE(rf::touched(reg::clocks::CLK_SYS_CTRL)) << "clk_sys was switched";
        EXPECT_EQ(rf::value(reg::clocks::CLK_PERI_CTRL), reg::clocks::CLK_PERI_ENABLE_XOSC);
        EXPECT_FALSE(ct::console_clk_on_pll());
        EXPECT_EQ(reg::uart::IBRD_115200, kickos::rp2xxx::pl011_ibrd(XOSC_HZ, BAUD));
        EXPECT_EQ(reg::uart::FBRD_115200, kickos::rp2xxx::pl011_fbrd(XOSC_HZ, BAUD));
    }

    TEST(ClockBringupRp2xxx, a_crystal_that_never_starts_stays_on_the_rosc)
    {
        struct Case
        {
            uintptr_t reg;
            uint32_t mask;
            char const* what;
        };
        Case const cases[] = {
            {reg::xosc::STATUS, reg::xosc::STATUS_STABLE, "the crystal is never stable"},
            {reg::clocks::CLK_REF_SELECTED, reg::clocks::CLK_REF_SELECTED_XOSC,
             "clk_ref never confirms the crystal"},
        };
        for (Case const& c : cases)
        {
            power_on();
            rf::plant_timeout(c.reg, c.mask);
            EXPECT_EQ(ct::bring_up(), reg::clocks::ROSC_NOMINAL_HZ) << c.what;
            EXPECT_FALSE(rf::touched(reg::pll::CS)) << c.what << ": PLL_SYS was started";
            EXPECT_FALSE(rf::touched(reg::clocks::CLK_SYS_CTRL)) << c.what;
            EXPECT_EQ(rf::value(reg::clocks::CLK_PERI_CTRL), reg::clocks::CLK_PERI_ENABLE_CLK_SYS)
                << c.what;
        }
    }
}
