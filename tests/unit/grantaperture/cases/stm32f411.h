// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The stm32f411 windows, from RM0383 Table 1.

#ifndef KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_STM32F411_H
#define KICKOS_TESTS_UNIT_GRANTAPERTURE_CASES_STM32F411_H

#include "../aperture_case.h"

constexpr ApertureCase CASES[] = {
    {"WWDG", 0x40002C00u, 0x400u, 0},
    {"IWDG", 0x40003000u, 0x400u, 0},
    {"PWR", 0x40007000u, 0x400u, -KOS_EINVAL},
    {"SYSCFG", 0x40013800u, 0x400u, -KOS_EINVAL},
    {"EXTI", 0x40013C00u, 0x400u, -KOS_EINVAL},
    {"GPIOA", 0x40020000u, 0x400u, -KOS_EINVAL},
    {"RCC", 0x40023800u, 0x400u, -KOS_EINVAL},
    {"FLASH", 0x40023C00u, 0x400u, -KOS_EINVAL},
    {"TIM2", 0x40000000u, 0x400u, -KOS_EPERM},
    {"USART2", 0x40004400u, 0x400u, 0},
    KICKOS_CASE_ARMV7M_MPU,
};

#endif
