// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The register contract the STM32 F0/F1/F3 parts share: one RCC block at 0x4002_1000
// whose PLL is programmed through RCC_CFGR (PLLSRC/PLLMUL), one FLASH_ACR, and one
// general-purpose TIM register map. The F2/F4/F7 line drives its PLL from RCC_PLLCFGR
// and shares none of this.
//
// A chip joins by shipping family.cmake and family_map.h. family_map.h states, in
// namespace kickos::stm32::chip, the values chip_stm32f1f3.cc reads: FLASH_LATENCY,
// PLL_SRC_ON, PLL_SRC_RDY, CFGR_CLEAR, CFGR_SET, POLL_LIMIT, CLK_TIMER_SR,
// CLK_COUNTER_TEARS, clk_counter(), USART_SR, USART_TXE, USART_DR, USART_CR1,
// CR1_TXEIE and CONSOLE_IRQ. That TU is compiled into the chip's OWN archive, once per
// chip, so every one of them reaches it as a compile-time constant.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F1F3_STM32F1F3_H
#define KICKOS_ARCH_ARM_CHIP_STM32F1F3_STM32F1F3_H

#include "regs.h" // arch/arm/common: kickos::arm::reg32

#include <stdint.h>

namespace kickos::stm32
{
    using arm::reg32;

    // RCC (RM0008 sec.7, RM0365 sec.9).
    constexpr uintptr_t RCC_BASE = 0x40021000;
    constexpr uintptr_t RCC_CR = RCC_BASE + 0x00;
    constexpr uintptr_t RCC_CFGR = RCC_BASE + 0x04;
    constexpr uintptr_t RCC_APB1ENR = RCC_BASE + 0x1C;
    constexpr uint32_t CR_PLLON = 1u << 24;
    constexpr uint32_t CR_PLLRDY = 1u << 25;
    constexpr uint32_t CFGR_SW_MASK = 0x3u << 0;
    constexpr uint32_t CFGR_SW_PLL = 0x2u << 0;  // SW=10: PLL as system clock
    constexpr uint32_t CFGR_SWS_MASK = 0x3u << 2;
    constexpr uint32_t CFGR_SWS_PLL = 0x2u << 2; // SWS=10: PLL used as system clock
    constexpr uint32_t CFGR_HPRE_DIV1 = 0x0u << 4;
    constexpr uint32_t CFGR_PPRE1_DIV2 = 0x4u << 8;
    constexpr uint32_t CFGR_PPRE2_DIV1 = 0x0u << 11;
    // HPRE[7:4], PPRE1[10:8], PPRE2[13:11], PLLSRC[16], PLLMUL[21:18]. PLLXTPRE[17]
    // is NOT here: it exists only where an HSE can feed the PLL.
    constexpr uint32_t CFGR_PLL_FIELDS =
        (0xFu << 4) | (0x7u << 8) | (0x7u << 11) | (0x1u << 16) | (0xFu << 18);

    // FLASH interface (RM0008 sec.3.3.3; the F3 shares the F0 layout).
    constexpr uintptr_t FLASH_ACR = 0x40022000;
    constexpr uint32_t ACR_LATENCY_MASK = 0x7u << 0;
    constexpr uint32_t ACR_LATENCY_2WS = 0x2u << 0;
    constexpr uint32_t ACR_PRFTBE = 1u << 4;

    // General-purpose TIM, offsets from the timer's own base.
    constexpr uintptr_t TIM_CR1 = 0x00;
    constexpr uintptr_t TIM_CR2 = 0x04;
    constexpr uintptr_t TIM_SMCR = 0x08;
    constexpr uintptr_t TIM_DIER = 0x0C;
    constexpr uintptr_t TIM_SR = 0x10;
    constexpr uintptr_t TIM_EGR = 0x14;
    constexpr uintptr_t TIM_CNT = 0x24;
    constexpr uintptr_t TIM_PSC = 0x28;
    constexpr uintptr_t TIM_ARR = 0x2C;
    constexpr uint32_t TIM_CR1_CEN = 1u << 0;
    constexpr uint32_t TIM_EGR_UG = 1u << 0;
    constexpr uint32_t TIM_DIER_UIE = 1u << 0; // update (overflow) interrupt enable
    constexpr uint32_t TIM_SR_UIF = 1u << 0;   // update (overflow) flag, rc_w0

    // OVER8=0 -> BRR = round(fck / baud). On the classic F1 USART that integer IS the
    // mantissa:fraction encoding, so one formula covers both register models.
    constexpr uint32_t usart_brr(uint32_t fck, uint32_t baud)
    {
        return (fck + baud / 2u) / baud;
    }

    // Bring SYSCLK onto the PLL described by family_map.h. False leaves the reset
    // HSI clock selected, so a part with a dead crystal or an unlocked PLL still
    // boots; the caller records the rate it actually got.
    bool clock_init();

    // Anchor arch_clock_now at the rate the raw counter advances at. Sole writer of
    // the epoch: a chip that retunes later has to reprice at the rate edge.
    void clock_anchor(uint32_t hz);
}

#endif
