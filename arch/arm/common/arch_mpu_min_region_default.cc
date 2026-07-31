// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The ARMv6-M / v7-M PMSA minimum region. A chip with no MPU at all (nRF51,
// STM32F103, STM32F302) defines 0. cmake/boot_arena.cmake SCRAPES the literal below,
// so it must stay a plain `return <int>;`.

#include <kickos/arch/arch.h>

#include <stddef.h>

extern "C" size_t arch_mpu_min_region(void)
{
    return 32u;
}
