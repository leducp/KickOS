// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411 general-purpose timer register map (RM0383 sec.13). Offsets are
// instance-relative; the monotonic time base uses TIM2 (mmap::TIM2_BASE), a
// 32-bit free-running counter on APB1.

#ifndef KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_TIM_H
#define KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_TIM_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::stm32f411::reg::tim
{
    constexpr uintptr_t CR1 = mmap::TIM2_BASE + 0x00u;
    constexpr uintptr_t DIER = mmap::TIM2_BASE + 0x0Cu;
    constexpr uintptr_t SR = mmap::TIM2_BASE + 0x10u;
    constexpr uintptr_t EGR = mmap::TIM2_BASE + 0x14u;
    constexpr uintptr_t CNT = mmap::TIM2_BASE + 0x24u;
    constexpr uintptr_t PSC = mmap::TIM2_BASE + 0x28u;
    constexpr uintptr_t ARR = mmap::TIM2_BASE + 0x2Cu;

    constexpr uint32_t CR1_CEN = 1u << 0;  // counter enable
    constexpr uint32_t EGR_UG = 1u << 0;   // update generation (latch PSC/ARR)
    constexpr uint32_t DIER_UIE = 1u << 0; // update (overflow) interrupt enable
    constexpr uint32_t SR_UIF = 1u << 0;   // update (overflow) flag, rc_w0
}

#endif // KICKOS_ARCH_ARM_CHIP_STM32F411_REGS_TIM_H
