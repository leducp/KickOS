// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// v7-M RASR encodes the size as ctz(size) - 1, so only a power of two is expressible.
// PMSAv8 (arch_arm_pmsav8.cc) and the K64F SYSMPU define 0. cmake/boot_arena.cmake
// SCRAPES the literal below, so it must stay a plain `return <int>;`.

#include <kickos/arch/arch.h>

extern "C" int arch_mpu_region_pow2(void)
{
    return 1;
}
