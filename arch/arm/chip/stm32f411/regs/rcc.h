// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 RCC register map (RM0383 sec.6): clock control + main PLL + bus
// prescalers + peripheral clock gates.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_RCC_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_RCC_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32f411::reg::rcc
{
    constexpr uintptr_t CR = mmap::RCC_BASE + 0x00u;       // sec.6.3.1 (RM lines 5129-5194)
    constexpr uintptr_t PLLCFGR = mmap::RCC_BASE + 0x04u;  // sec.6.3.2 (RM lines 5227-5324)
    constexpr uintptr_t CFGR = mmap::RCC_BASE + 0x08u;     // sec.6.3.3 (RM lines 5333-5474)
    constexpr uintptr_t AHB1ENR = mmap::RCC_BASE + 0x30u;
    constexpr uintptr_t APB1ENR = mmap::RCC_BASE + 0x40u;
    constexpr uintptr_t APB2ENR = mmap::RCC_BASE + 0x44u;
    constexpr uintptr_t APB1LPENR = mmap::RCC_BASE + 0x60u; // keep-clocked-in-sleep gates

    // Peripheral clock-enable bits.
    constexpr uint32_t AHB1ENR_GPIOAEN = 1u << 0;
    constexpr uint32_t APB1ENR_USART2EN = 1u << 17;
    constexpr uint32_t APB1ENR_TIM2EN = 1u << 0; // also the APB1LPENR TIM2 bit
    constexpr uint32_t APB2ENR_SPI1EN = 1u << 12;

    // RCC_CR flags (RM lines 5157-5194).
    constexpr uint32_t CR_HSEON = 1u << 16;
    constexpr uint32_t CR_HSERDY = 1u << 17;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;

    // RCC_CFGR: system-clock switch + bus prescalers (RM lines 5419-5474).
    constexpr uint32_t CFGR_SW_MASK = 0x3u << 0;     // SW[1:0] system-clock select
    constexpr uint32_t CFGR_SW_PLL = 0x2u << 0;      // SW=10: PLL as SYSCLK
    constexpr uint32_t CFGR_SWS_PLL = 0x2u << 2;     // SWS=10: PLL is SYSCLK (readback)
    constexpr uint32_t CFGR_HPRE_DIV1 = 0x0u << 4;   // AHB  = SYSCLK/1
    constexpr uint32_t CFGR_PPRE1_DIV2 = 0x4u << 10; // APB1 = HCLK/2 (<=42 MHz)
    constexpr uint32_t CFGR_PPRE2_DIV1 = 0x0u << 13; // APB2 = HCLK/1 (<=84 MHz)

    // RCC_PLLCFGR fields (RM lines 5232-5324). PLLM is board-derived (HSE/1 MHz),
    // so it has no fixed constant here; PLLN/PLLP/PLLQ are board-independent.
    constexpr uint32_t PLLCFGR_PLLSRC_HSE = 1u << 22;  // HSE as PLL entry (RM line 5266)
    constexpr uint32_t PLLCFGR_PLLP_DIV4 = 0x1u << 16; // PLLP=01 -> /4 (RM line 5283)
    constexpr uint32_t PLLCFGR_PLLM_SHIFT = 0u;
    constexpr uint32_t PLLCFGR_PLLN_SHIFT = 6u;
    constexpr uint32_t PLLCFGR_PLLQ_SHIFT = 24u;
    constexpr uint32_t PLLN = 336u; // VCO_out = 1 MHz * 336 = 336 MHz
    constexpr uint32_t PLLQ = 7u;   // PLL48 = 336 / 7 = 48 MHz
}

#endif
