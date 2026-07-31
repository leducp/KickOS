// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// PMSA needs a power-of-two size >= the granule with the base naturally aligned to it:
// the RBAR base masking in kickos_arm_mpu_program assumes exactly that. A granule of 0
// means the chip has no MPU, where only the 16-byte arena rule applies.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

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
