// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SYSCLK onto the HSE-fed main PLL at 84 MHz, APB1 at 42 MHz (RM0383 sec.6). Every failure stays
// on the 16 MHz HSI with APB1 undivided, and the rates returned name the clocks the part runs.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_CLOCK_BRINGUP_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_CLOCK_BRINGUP_H

#include <kickos/arch/bounded_wait.h>
#include <kickos/io/mmio.h>

#include <stdint.h>

#include "regs/flash.h"
#include "regs/rcc.h"

namespace kickos::stm32f411::clocktree
{
    constexpr uint32_t HSI_HZ = 16000000u;
    constexpr uint32_t SYSCLK_PLL_HZ = 84000000u;
    constexpr uint32_t PCLK1_PLL_HZ = 42000000u; // APB1 = 84/2

    struct Landed
    {
        uint32_t sysclk_hz;
        uint32_t pclk1_hz;
    };

    // `pllcfgr` is the board's PLLCFGR word, PLLM taken from its crystal.
    static inline Landed bring_up(uint32_t pllcfgr)
    {
        namespace rcc = reg::rcc;
        namespace flash = reg::flash;
        Landed const reset = {HSI_HZ, HSI_HZ};

        // Flash wait states MUST widen before the core runs faster, else the first over-speed
        // fetch faults.
        r32(flash::ACR) =
            flash::ACR_LATENCY_2WS | flash::ACR_PRFTEN | flash::ACR_ICEN | flash::ACR_DCEN;

        r32(rcc::CR) |= rcc::CR_HSEON;
        if (not kickos::arch::wait_set(r32(rcc::CR), rcc::CR_HSERDY))
        {
            return reset;
        }

        // PLL config bits are writable only while the PLL is off.
        r32(rcc::PLLCFGR) = pllcfgr;

        r32(rcc::CR) |= rcc::CR_PLLON;
        if (not kickos::arch::wait_set(r32(rcc::CR), rcc::CR_PLLRDY))
        {
            return reset;
        }

        // After the lock, or a PLL fallback runs the USART at HSI/2 while the rate names the HSI.
        // Before the switch, so APB1<=42 / APB2<=84 always hold.
        r32(rcc::CFGR) = rcc::CFGR_HPRE_DIV1 | rcc::CFGR_PPRE1_DIV2 | rcc::CFGR_PPRE2_DIV1;

        r32(rcc::CFGR) = (r32(rcc::CFGR) & ~rcc::CFGR_SW_MASK) | rcc::CFGR_SW_PLL;
        if (not kickos::arch::wait_set(r32(rcc::CFGR), rcc::CFGR_SWS_PLL))
        {
            // Back to the HSI, then APB1 to /1. APB1 drops its /2 only once SYSCLK is confirmed
            // off the PLL, or it runs past 42 MHz.
            r32(rcc::CFGR) = rcc::CFGR_HPRE_DIV1 | rcc::CFGR_PPRE1_DIV2 | rcc::CFGR_PPRE2_DIV1;
            if (kickos::arch::wait_clear(r32(rcc::CFGR), rcc::CFGR_SWS_MASK))
            {
                r32(rcc::CFGR) = 0;
            }
            return reset;
        }

        return {SYSCLK_PLL_HZ, PCLK1_PLL_HZ};
    }
}

#endif
