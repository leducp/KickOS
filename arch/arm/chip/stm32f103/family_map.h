// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F103C8 answers to the STM32 F0/F1/F3 family contract (../stm32f1f3/stm32f1f3.h),
// clean-room from RM0008. Read by chip_stm32f1f3.cc and by chip_stm32f103.cc.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F103_FAMILY_MAP_H
#define KICKOS_ARCH_ARM_CHIP_STM32F103_FAMILY_MAP_H

#include "irq.h"
#include "stm32f1f3.h"

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32::chip
{
    // Clock tree: HSE(8 MHz) x PLL9. SYSCLK = HCLK = PCLK2 = 72 MHz,
    // PCLK1 = 36 MHz (its max).
    constexpr uint32_t FLASH_LATENCY = ACR_LATENCY_2WS; // 48 < SYSCLK <= 72 MHz
    constexpr uint32_t PLL_SRC_ON = 1u << 16;  // RCC_CR.HSEON
    constexpr uint32_t PLL_SRC_RDY = 1u << 17; // RCC_CR.HSERDY
    constexpr uint32_t CFGR_PLLSRC_HSE = 1u << 16;
    constexpr uint32_t CFGR_PLLXTPRE = 1u << 17;  // HSE divider ahead of the PLL
    constexpr uint32_t CFGR_PLLMUL9 = 0x7u << 18; // PLLMUL=0111: input x9
    constexpr uint32_t CFGR_CLEAR = CFGR_PLL_FIELDS | CFGR_PLLXTPRE;
    constexpr uint32_t CFGR_SET = CFGR_HPRE_DIV1 | CFGR_PPRE1_DIV2 | CFGR_PPRE2_DIV1
                                  | CFGR_PLLSRC_HSE | CFGR_PLLMUL9;
    constexpr uint32_t POLL_LIMIT = 0x10000u;

    // Monotonic time base (RM0008 sec.15). Every F1 general-purpose timer is
    // 16-bit, so a single free-runner would wrap every ~0.9 ms and lose whole
    // wraps between clock reads. TIM2 (master) counts at the timer kernel clock
    // and emits TRGO on each overflow; TIM3 (slave, ext clock mode 1 off
    // ITR1=TIM2) counts those, so {TIM3:TIM2} is one 32-bit counter wrapping
    // every ~59 s. Neither collides with the tickless SysTick.
    constexpr uintptr_t CLK_TIMER_SR = mmap::TIM3_BASE + TIM_SR;
    constexpr bool CLK_COUNTER_TEARS = true;
    constexpr int CLK_TIMER_IRQ = irq::TIM3_GLOBAL;

    // Re-read the TIM3 high half last: a stable high half validates the low half
    // against a straddled TIM2 roll-under. The timers keep counting whatever the
    // IRQ mask says, hence the retry rather than a single pass.
    inline uint32_t clk_counter()
    {
        uint32_t hi = reg32(mmap::TIM3_BASE + TIM_CNT) & 0xFFFFu;
        uint32_t lo;
        while (true)
        {
            lo = reg32(mmap::TIM2_BASE + TIM_CNT) & 0xFFFFu;
            uint32_t hi2 = reg32(mmap::TIM3_BASE + TIM_CNT) & 0xFFFFu;
            if (hi2 == hi)
            {
                break;
            }
            hi = hi2;
        }
        return (hi << 16) | lo;
    }

    // Console: USART1, APB2, the classic SR/DR model.
    constexpr uintptr_t USART_SR = mmap::USART1_BASE + 0x00;
    constexpr uintptr_t USART_DR = mmap::USART1_BASE + 0x04;
    constexpr uintptr_t USART_BRR = mmap::USART1_BASE + 0x08;
    constexpr uintptr_t USART_CR1 = mmap::USART1_BASE + 0x0C;
    constexpr uint32_t USART_TXE = 1u << 7;
    constexpr uint32_t CR1_RE = 1u << 2;
    constexpr uint32_t CR1_TE = 1u << 3;
    constexpr uint32_t CR1_TXEIE = 1u << 7;
    constexpr uint32_t CR1_UE = 1u << 13;
    // USART1 global interrupt (RX/TX combined). Only TXEIE is armed, so the
    // drain ISR is the sole source.
    constexpr int CONSOLE_IRQ = irq::USART1_GLOBAL;
}

#endif
