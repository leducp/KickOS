// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 USART register map (RM0383 sec.19), classic SR/DR layout. Offsets
// are instance-relative; the console uses USART2 (mmap::USART2_BASE) on APB1.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_USART_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_USART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32f411::reg::usart
{
    constexpr uintptr_t SR = mmap::USART2_BASE + 0x00u;
    constexpr uintptr_t DR = mmap::USART2_BASE + 0x04u;
    constexpr uintptr_t BRR = mmap::USART2_BASE + 0x08u;
    constexpr uintptr_t CR1 = mmap::USART2_BASE + 0x0Cu;

    constexpr uint32_t SR_TXE = 1u << 7;   // TX data register empty
    constexpr uint32_t CR1_RE = 1u << 2;   // receiver enable
    constexpr uint32_t CR1_TE = 1u << 3;   // transmitter enable
    constexpr uint32_t CR1_TXEIE = 1u << 7; // TXE interrupt enable
    constexpr uint32_t CR1_UE = 1u << 13;  // USART enable

    // OVER8=0: BRR = fPCLK1 / baud, with BRR[15:4]=mantissa, BRR[3:0]=fraction/16
    // (RM lines 27814-27830, 28373-28378). Rounded to nearest at compute time.
    constexpr uint32_t BAUD_115200 = 115200u;
}

#endif
