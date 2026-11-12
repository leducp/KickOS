// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6's clock bring-up over a TIMG0 T0 that counts PLL_F80M / 2 against an MTIME that
// counts CPU_CLK (TRM ch.8, ch.14). The CPU must reach the PLL only when the PLL counts 40 MHz.

#include "arch/riscv/chip/esp32c6/clock_bringup.h"

#include "regfile.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    namespace rf = kickos::testfix::regfile;
    namespace c6 = kickos::esp32c6;
    namespace pcr = kickos::esp32c6::reg::pcr;
    namespace timg = kickos::esp32c6::reg::timg;
    namespace clint = kickos::esp32c6::reg::clint;
    namespace cache = kickos::esp32c6::reg::cache;

    constexpr uintptr_t T0CONFIG = c6::mmap::TIMG0_BASE + timg::T0CONFIG;
    constexpr uintptr_t T0LO = c6::mmap::TIMG0_BASE + timg::T0LO;
    constexpr uintptr_t T0UPDATE = c6::mmap::TIMG0_BASE + timg::T0UPDATE;

    constexpr uint32_t XTAL_MHZ = 40u;
    constexpr uint32_t XTAL_HZ = XTAL_MHZ * 1000000u;
    constexpr uint32_t SYSCLK_XTAL = XTAL_MHZ << pcr::CLK_XTAL_FREQ_S;
    // 1 us of a 40 MHz CPU_CLK per MTIME read.
    constexpr uint32_t MTIME_STEP = 40u;
    constexpr uint32_t CLK_FOUND = 0x5u;
    constexpr uint32_t T0_FOUND = timg::T0_EN | 0x123u;

    // PLL_F80M_CLK / 2 as T0 sees it once its clock is on the PLL; 0 for a PLL that does not run.
    uint32_t g_t0_hz = 0;
    uint64_t g_mtime = 0;
    uint64_t g_t0_ticks = 0;
    uint32_t g_flushes = 0;
    uint32_t g_sysclk_at_flush = 0;
    uint32_t g_spi0_at_flush = 0;

    bool t0_counts()
    {
        uint32_t const clk = rf::value(pcr::TG0_TIMER_CLK_CONF);
        uint32_t const sel = (clk >> pcr::TG0_TIMER_CLK_SEL_S) & pcr::TG0_TIMER_CLK_SEL_MASK;
        return g_t0_hz != 0u and (clk & pcr::TG0_TIMER_CLK_EN) != 0u
               and sel == pcr::TG0_TIMER_CLK_SEL_PLL_F80M
               and (rf::value(T0CONFIG) & timg::T0_EN) != 0u;
    }

    // Time moves on each MTIME read, and T0 with it while its clock runs. A latch completes only
    // in a running T0 clock domain.
    void hardware(uintptr_t address)
    {
        if (address == clint::MTIME and (rf::value(clint::MTIMECTL) & clint::MTIMECTL_MTCE) != 0u)
        {
            g_mtime += MTIME_STEP;
            if (t0_counts())
            {
                g_t0_ticks += static_cast<uint64_t>(MTIME_STEP) * g_t0_hz / XTAL_HZ;
            }
            rf::seed(clint::MTIME, static_cast<uint32_t>(g_mtime));
        }
        if (address == T0UPDATE and (rf::value(T0UPDATE) & timg::T0_UPDATE) != 0u and t0_counts())
        {
            rf::seed(T0LO, static_cast<uint32_t>(g_t0_ticks));
            rf::seed(T0UPDATE, 0u);
        }
    }

    void flush()
    {
        g_flushes++;
        g_sysclk_at_flush = rf::value(pcr::SYSCLK_CONF);
        g_spi0_at_flush = rf::value(cache::SPI0_CLOCK);
    }

    void power_on(uint32_t t0_hz, uint32_t sysclk)
    {
        rf::reset();
        rf::on_access(hardware);
        g_t0_hz = t0_hz;
        g_mtime = 0;
        g_t0_ticks = 0;
        g_flushes = 0;
        g_sysclk_at_flush = 0;
        g_spi0_at_flush = 0;
        rf::seed(pcr::SYSCLK_CONF, sysclk);
        rf::seed(pcr::TG0_TIMER_CLK_CONF, CLK_FOUND);
        rf::seed(T0CONFIG, T0_FOUND);
    }

    uint32_t cpu_hz()
    {
        uint32_t const sys = rf::value(pcr::SYSCLK_CONF);
        uint32_t const cpu = rf::value(pcr::CPU_FREQ_CONF);
        return c6::cpu_clk_hz((sys >> pcr::SOC_CLK_SEL_S) & pcr::SOC_CLK_SEL_MASK,
                              (sys >> pcr::CLK_XTAL_FREQ_S) & pcr::CLK_XTAL_FREQ_MASK,
                              (cpu >> pcr::CPU_LS_DIV_NUM_S) & pcr::CPU_LS_DIV_NUM_MASK,
                              (cpu >> pcr::CPU_HS_DIV_NUM_S) & pcr::CPU_HS_DIV_NUM_MASK,
                              (cpu & pcr::CPU_HS_120M_FORCE) != 0u);
    }

    TEST(ClockBringupEsp32c6, a_pll_counting_40_mhz_becomes_the_cpu_root)
    {
        power_on(c6::PLL_PROBE_HZ, SYSCLK_XTAL);
        rf::seed(pcr::CPU_FREQ_CONF, pcr::CPU_HS_120M_FORCE | (2u << pcr::CPU_HS_DIV_NUM_S));
        c6::clocktree::bring_up(flush);
        EXPECT_EQ(cpu_hz(), 160000000u);
        EXPECT_EQ((rf::value(pcr::AHB_FREQ_CONF) >> pcr::AHB_HS_DIV_NUM_S) & pcr::AHB_HS_DIV_NUM_MASK,
                  pcr::AHB_HS_DIV_NUM_BY_4);
        EXPECT_EQ((rf::value(cache::PCR_MSPI_CLK_CONF) >> cache::MSPI_FAST_HS_DIV_S)
                      & cache::MSPI_FAST_HS_DIV_MASK,
                  cache::MSPI_FAST_HS_DIV_80M);
        EXPECT_EQ(g_flushes, 1u);
        EXPECT_EQ(g_sysclk_at_flush, SYSCLK_XTAL) << "the console was flushed after the switch";
        EXPECT_NE(g_spi0_at_flush, 0u) << "the flash clock was slowed after the switch";
        EXPECT_EQ(rf::value(pcr::TG0_TIMER_CLK_CONF), CLK_FOUND);
        EXPECT_EQ(rf::value(T0CONFIG), T0_FOUND);
    }

    TEST(ClockBringupEsp32c6, a_pll_not_seen_at_40_mhz_leaves_the_cpu_on_the_crystal)
    {
        struct Case
        {
            uint32_t t0_hz;
            char const* what;
        };
        Case const cases[] = {
            {0u, "the PLL does not run"},
            {c6::PLL_PROBE_HZ / 2u, "the PLL runs at half its rate"},
            {c6::PLL_PROBE_HZ + c6::PLL_PROBE_HZ / 32u, "the PLL runs 1/32 fast"},
        };
        for (Case const& c : cases)
        {
            power_on(c.t0_hz, SYSCLK_XTAL);
            c6::clocktree::bring_up(flush);
            EXPECT_EQ(cpu_hz(), XTAL_HZ) << c.what;
            EXPECT_EQ(rf::value(pcr::SYSCLK_CONF), SYSCLK_XTAL) << c.what;
            EXPECT_FALSE(rf::touched(pcr::AHB_FREQ_CONF)) << c.what;
            EXPECT_FALSE(rf::touched(cache::SPI0_CLOCK)) << c.what;
            EXPECT_EQ(g_flushes, 0u) << c.what;
            EXPECT_EQ(rf::value(pcr::TG0_TIMER_CLK_CONF), CLK_FOUND) << c.what;
            EXPECT_EQ(rf::value(T0CONFIG), T0_FOUND) << c.what;
        }
    }

    TEST(ClockBringupEsp32c6, a_boot_on_the_pll_sets_the_dividers_without_a_probe)
    {
        uint32_t const sysclk = SYSCLK_XTAL | (c6::SOC_CLK_SEL_PLL << pcr::SOC_CLK_SEL_S);
        power_on(0u, sysclk);
        rf::seed(pcr::CPU_FREQ_CONF, 1u << pcr::CPU_HS_DIV_NUM_S);
        c6::clocktree::bring_up(flush);
        EXPECT_EQ(cpu_hz(), 160000000u);
        EXPECT_EQ(rf::value(pcr::SYSCLK_CONF), sysclk);
        EXPECT_FALSE(rf::touched(T0CONFIG));
        EXPECT_EQ(g_flushes, 0u);
    }

    TEST(ClockBringupEsp32c6, a_boot_on_rc_fast_is_left_alone)
    {
        uint32_t const sysclk = SYSCLK_XTAL | (2u << pcr::SOC_CLK_SEL_S);
        power_on(c6::PLL_PROBE_HZ, sysclk);
        c6::clocktree::bring_up(flush);
        EXPECT_EQ(rf::value(pcr::SYSCLK_CONF), sysclk);
        EXPECT_FALSE(rf::touched(pcr::AHB_FREQ_CONF));
        EXPECT_FALSE(rf::touched(pcr::CPU_FREQ_CONF));
        EXPECT_FALSE(rf::touched(T0CONFIG));
    }
}
