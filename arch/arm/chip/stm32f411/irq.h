// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 NVIC interrupt/vector numbers (RM0383 vector table). Line = IPSR - 16.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_IRQ_H

namespace kickos::stm32f411::irq
{
    enum irq_num
    {
        TIM2_IRQ = 28,   // TIM2 global (update/overflow observer)
        USART2_IRQ = 38, // USART2 global (RX/TX combined); only TXEIE armed
    };
}

#endif
