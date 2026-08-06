// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 (F411E-DISCO + Black Pill, Cortex-M4F) peripheral base addresses,
// clean-room from RM0383. Bases only; register offsets + fields live in
// regs/<periph>.h. Hand-rolled, no vendor HAL/CMSIS.

#ifndef KICKOS_CHIP_MMAP_H
#define KICKOS_CHIP_MMAP_H

#include <stdint.h>

namespace kickos::stm32f411::mmap
{
    constexpr uintptr_t TIM2_BASE = 0x40000000u;   // monotonic time base (RM sec.13)
    constexpr uintptr_t USART2_BASE = 0x40004400u; // console, APB1 (RM sec.19)
    constexpr uintptr_t SPI1_BASE = 0x40013000u;   // APB2 (RM sec.20)

    // GPIO port controllers: GPIOA + port*0x400 (A=0..H). AHB1.
    constexpr uintptr_t GPIOA_BASE = 0x40020000u;
    constexpr uintptr_t GPIO_STRIDE = 0x400u;
    constexpr uintptr_t GPIOC_BASE = 0x40020800u; // Black Pill LED (PC13)
    constexpr uintptr_t GPIOD_BASE = 0x40020C00u; // F411E-DISCO LED (PD12)

    constexpr uintptr_t RCC_BASE = 0x40023800u;   // clock control + PLL + gates (RM sec.6)
    constexpr uintptr_t FLASH_BASE = 0x40023C00u; // flash interface / ACR (RM sec.3.8.1)

    // Core (armv7m-shared, not RM0383): CPACR coprocessor-access control, used to
    // enable the FPU. NVIC enables go through the shared armv7m arch layer, not a
    // base defined here.
    constexpr uintptr_t SCB_CPACR = 0xE000ED88u;
}

#endif // KICKOS_CHIP_MMAP_H
