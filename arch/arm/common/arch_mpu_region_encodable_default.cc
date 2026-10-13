// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// PMSA needs a power-of-two size >= the granule with the base naturally aligned to it:
// the RBAR base masking in kickos_arm_mpu_program assumes exactly that. A granule of 0
// means the chip has no MPU, where only the 16-byte arena rule applies.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#include <stddef.h>
#include <stdint.h>

// Every ARM region unit names 32 bytes at the least: the PMSAv6/v7 SIZE floor, the PMSAv8 and
// SYSMPU 32-byte bounds the encoders test.
static_assert(KICKOS_CHIP_REGION_GRANULE == 0 or KICKOS_CHIP_REGION_GRANULE == 32,
              "the chip file's protection granule is not the 32 bytes every ARM region unit names");

extern "C" bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size == 0)
    {
        return false;
    }
    size_t const min = arch_mpu_min_region();
    if (min == 0)
    {
        return (base & 15u) == 0 and (size & 15u) == 0;
    }
    if (size < min or (size & (size - 1)) != 0)
    {
        return false;
    }
    return (base & (size - 1)) == 0;
}
