// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F302R8 answers to the STM32 F0/F1/F3 family contract (../stm32f1f3/stm32f1f3.h),
// clean-room from RM0365. Read by chip_stm32f1f3.cc and by chip_stm32f302.cc.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F302_FAMILY_MAP_H
#define KICKOS_ARCH_ARM_CHIP_STM32F302_FAMILY_MAP_H

#include "stm32f1f3.h"

#include <stdint.h>

namespace kickos::stm32::chip
{
    // Clock tree: HSI/2 (4 MHz) x PLL16. HCLK = PCLK2 = 64 MHz, PCLK1 = 32 MHz
    // (its 36 MHz max). The Nucleo-F302R8 carries no crystal, so nothing here may
    // depend on an HSE or on an MCO solder bridge.
    constexpr uint32_t FLASH_LATENCY = ACR_LATENCY_2WS; // 48 < SYSCLK <= 72 MHz
    constexpr uint32_t PLL_SRC_ON = 1u << 0;  // RCC_CR.HSION
    constexpr uint32_t PLL_SRC_RDY = 1u << 1; // RCC_CR.HSIRDY
    constexpr uint32_t CFGR_PLLSRC_HSI_DIV2 = 0x0u << 16;
    constexpr uint32_t CFGR_PLLMUL16 = 0xEu << 18;
    constexpr uint32_t CFGR_CLEAR = CFGR_PLL_FIELDS;
    constexpr uint32_t CFGR_SET = CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1
                                  | CFGR_PLLSRC_HSI_DIV2 | CFGR_PLLMUL16;
    constexpr uint32_t POLL_LIMIT = 0x100000u;

    // Monotonic time base (RM0365 sec.21): TIM2 is 32-bit here, free-running and
    // wrapping every ~67 s, and does not collide with the tickless SysTick.
    constexpr uintptr_t TIM2_BASE = 0x40000000;
    constexpr uintptr_t CLK_TIMER_SR = TIM2_BASE + TIM_SR;
    constexpr bool CLK_COUNTER_TEARS = false;
    constexpr int CLK_TIMER_IRQ = 28; // NVIC position 28 = TIM2 (RM0365)

    inline uint32_t clk_counter() { return reg32(TIM2_BASE + TIM_CNT); }

    // Console: USART2 on PA2/PA3 (AF7, the ST-LINK VCP), APB1. The NEWER USART
    // model (ISR/TDR), though CR1.TXEIE is bit 7 as on the classic one.
    constexpr uintptr_t USART2_BASE = 0x40004400;
    constexpr uintptr_t USART_CR1 = USART2_BASE + 0x00;
    constexpr uintptr_t USART_BRR = USART2_BASE + 0x0C;
    constexpr uintptr_t USART_SR = USART2_BASE + 0x1C; // ISR
    constexpr uintptr_t USART_DR = USART2_BASE + 0x28; // TDR
    constexpr uint32_t USART_TC = 1u << 6; // transmission complete: shift register idle too
    constexpr uint32_t USART_TXE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 0;
    constexpr uint32_t CR1_RE = 1u << 2;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_TXEIE = 1u << 7;
    // USART2 global interrupt (RX/TX combined). Only TXEIE is armed, so the
    // drain ISR is the sole source.
    constexpr int CONSOLE_IRQ = 38;
}

#endif
