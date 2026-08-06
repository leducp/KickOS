// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 GPIO register map (RM0383 sec.8). Offsets are port-relative; add to
// a mmap:: port base (GPIOA_BASE + port*GPIO_STRIDE).

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_GPIO_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_GPIO_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::stm32f411::reg::gpio
{
    constexpr uintptr_t MODER = 0x00u; // 2 bits/pin
    constexpr uintptr_t AFRL = 0x20u;  // 4 bits/pin, pins 0-7
    constexpr uintptr_t AFRH = 0x24u;  // 4 bits/pin, pins 8-15
    constexpr uintptr_t BSRR = 0x18u;  // atomic set [15:0] / reset [31:16]

    // MODER 2-bit field encodings.
    constexpr uint32_t MODER_OUTPUT = 0x1u; // general-purpose output
    constexpr uint32_t MODER_AF = 0x2u;     // alternate function

    // AFRL/AFRH 4-bit field: USART2 pins (PA2/PA3) are AF7.
    constexpr uint32_t AF7 = 7u;
}

#endif // KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_GPIO_H
