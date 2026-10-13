// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// The region rule the chip file states for its protection unit (platform/<chip>/chip.yaml).

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#include <stddef.h>

size_t arch_mpu_min_region(void)
{
    return KICKOS_CHIP_REGION_GRANULE;
}

int arch_mpu_region_pow2(void)
{
    return KICKOS_CHIP_REGION_POW2;
}
