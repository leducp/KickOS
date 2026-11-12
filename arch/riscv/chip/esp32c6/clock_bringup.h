// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// CPU_CLK onto the PLL at 160 MHz, AHB_CLK at 40 MHz (ESP32-C6 TRM ch.8), and the crystal kept
// when the PLL is not seen running.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLOCK_BRINGUP_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLOCK_BRINGUP_H

#include <kickos/io/mmio.h>
#include <kickos/poll_bound.h>

#include <stdint.h>

#include "mtime_conv.h"
#include "regs/cache.h"
#include "regs/clint.h"
#include "regs/pcr.h"
#include "regs/timg.h"

namespace kickos::esp32c6
{
    // A register synchronisation that never completes must cost a bounded delay.
    constexpr uint32_t REG_UPDATE_SPIN_MAX = 200000u;
}

namespace kickos::esp32c6::clocktree
{
    // T0's latch completes in its own clock domain, so a stopped clock leaves T0UPDATE set.
    static inline bool timg0_t0_latch(uint32_t& lo)
    {
        r32(mmap::TIMG0_BASE + reg::timg::T0UPDATE) = reg::timg::T0_UPDATE;
        for (uint32_t i = 0; i < REG_UPDATE_SPIN_MAX; i++)
        {
            if ((r32(mmap::TIMG0_BASE + reg::timg::T0UPDATE) & reg::timg::T0_UPDATE) == 0u)
            {
                lo = r32(mmap::TIMG0_BASE + reg::timg::T0LO);
                return true;
            }
        }
        return false;
    }

    // Counts PLL_F80M_CLK on TIMG0's T0 for 1 ms of MTIME at cpu_hz, then hands T0 and its clock
    // back as found. The TRM names no PLL lock bit, and a PLL selected while it does not run stops
    // the core.
    static inline bool pll_f80m_runs(uint32_t cpu_hz)
    {
        namespace timg = reg::timg;
        uintptr_t const t0 = mmap::TIMG0_BASE;
        uint32_t const clk_found = r32(reg::pcr::TG0_TIMER_CLK_CONF);
        uint32_t const t0_found = r32(t0 + timg::T0CONFIG);
        uint32_t const t0_div2 = timg::T0_INCREASE | (1u << timg::T0_DIVIDER_S);

        r32(t0 + timg::T0CONFIG) = t0_div2; // the prescaler is rewritten only with T0 stopped
        r32(t0 + timg::T0CONFIG) = t0_div2 | timg::T0_DIVCNT_RST;
        uint32_t clk =
            clk_found & ~(reg::pcr::TG0_TIMER_CLK_SEL_MASK << reg::pcr::TG0_TIMER_CLK_SEL_S);
        clk |= (reg::pcr::TG0_TIMER_CLK_SEL_PLL_F80M << reg::pcr::TG0_TIMER_CLK_SEL_S)
               | reg::pcr::TG0_TIMER_CLK_EN;
        r32(reg::pcr::TG0_TIMER_CLK_CONF) = clk;
        r32(t0 + timg::T0CONFIG) = t0_div2 | timg::T0_EN;
        r32(reg::clint::MTIMECTL) |= reg::clint::MTIMECTL_MTCE;

        uint32_t probe_from = 0;
        uint32_t probe_to = 0;
        bool latched = timg0_t0_latch(probe_from);
        uint32_t const mtime_from = r32(reg::clint::MTIME);
        uint32_t const window = cpu_hz / 1000u;
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
        {
            if (r32(reg::clint::MTIME) - mtime_from >= window)
            {
                break;
            }
        }
        latched = timg0_t0_latch(probe_to) and latched;
        uint32_t const mtime_to = r32(reg::clint::MTIME);

        r32(t0 + timg::T0CONFIG) = t0_div2;
        r32(reg::pcr::TG0_TIMER_CLK_CONF) = clk_found;
        r32(t0 + timg::T0CONFIG) = (t0_found & ~timg::T0_EN) | timg::T0_DIVCNT_RST;
        r32(t0 + timg::T0CONFIG) = t0_found;

        return latched and pll_probe_alive(probe_to - probe_from, mtime_to - mtime_from, cpu_hz);
    }

    // SPI0's flash clock at 80 MHz / 2 once the PLL is the root, the image header's 40 MHz. The
    // reset MSPI_FAST_CLK on the PLL, 120 MHz, outruns the SPI0 timing the ROM set.
    static inline void flash_clock_for_pll(void)
    {
        namespace c = reg::cache;
        r32(c::SPI0_CLOCK) = (1u << c::CLKCNT_N_S) | (0u << c::CLKCNT_H_S) | (1u << c::CLKCNT_L_S);
        uint32_t const mspi =
            r32(c::PCR_MSPI_CLK_CONF) & ~(c::MSPI_FAST_HS_DIV_MASK << c::MSPI_FAST_HS_DIV_S);
        r32(c::PCR_MSPI_CLK_CONF) = mspi | (c::MSPI_FAST_HS_DIV_80M << c::MSPI_FAST_HS_DIV_S);
    }

    // A crystal boot switches only once the PLL is seen running, and otherwise stays on the
    // crystal. console_flush runs right before the switch. The UART's function clock is its own
    // PCR selection, which no step here moves.
    static inline void bring_up(void (*console_flush)(void))
    {
        uint32_t const sys = r32(reg::pcr::SYSCLK_CONF);
        uint32_t const sel = (sys >> reg::pcr::SOC_CLK_SEL_S) & reg::pcr::SOC_CLK_SEL_MASK;
        if (sel == SOC_CLK_SEL_XTAL)
        {
            uint32_t const cpu = r32(reg::pcr::CPU_FREQ_CONF);
            uint32_t const xtal_hz = cpu_clk_hz(
                SOC_CLK_SEL_XTAL, (sys >> reg::pcr::CLK_XTAL_FREQ_S) & reg::pcr::CLK_XTAL_FREQ_MASK,
                (cpu >> reg::pcr::CPU_LS_DIV_NUM_S) & reg::pcr::CPU_LS_DIV_NUM_MASK, 0u, false);
            if (not pll_f80m_runs(xtal_hz))
            {
                return;
            }
        }
        else if (sel != SOC_CLK_SEL_PLL)
        {
            return;
        }
        // The AHB divider first: CPU_CLK must stay a multiple of AHB_CLK at every step.
        uint32_t ahb = r32(reg::pcr::AHB_FREQ_CONF);
        ahb &= ~(reg::pcr::AHB_HS_DIV_NUM_MASK << reg::pcr::AHB_HS_DIV_NUM_S);
        r32(reg::pcr::AHB_FREQ_CONF) =
            ahb | (reg::pcr::AHB_HS_DIV_NUM_BY_4 << reg::pcr::AHB_HS_DIV_NUM_S);
        uint32_t cpu = r32(reg::pcr::CPU_FREQ_CONF);
        cpu &= ~((reg::pcr::CPU_HS_DIV_NUM_MASK << reg::pcr::CPU_HS_DIV_NUM_S)
                 | reg::pcr::CPU_HS_120M_FORCE);
        r32(reg::pcr::CPU_FREQ_CONF) = cpu;
        if (sel == SOC_CLK_SEL_XTAL)
        {
            flash_clock_for_pll();
            console_flush();
            uint32_t const to_pll = sys & ~(reg::pcr::SOC_CLK_SEL_MASK << reg::pcr::SOC_CLK_SEL_S);
            r32(reg::pcr::SYSCLK_CONF) = to_pll | (SOC_CLK_SEL_PLL << reg::pcr::SOC_CLK_SEL_S);
        }
    }
}

#endif
