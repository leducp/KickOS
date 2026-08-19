// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sim: mprotect takes the addresses themselves, so there is nothing to pre-encode. The
// image carries only which regions the arena can enforce, which is what keeps the seam's
// refusal answer the same shape here as on silicon.

#ifndef KICKOS_ARCH_MPU_ENCODED_H
#define KICKOS_ARCH_MPU_ENCODED_H

#include <stdint.h>

// Descriptor slots the image carries. kernel/include/kickos/mpuset.h static_asserts that
// KICKOS_MPU_MAX_REGIONS fits.
#define ARCH_MPU_ENCODED_SLOTS 8

struct arch_mpu_encoded
{
    uint32_t seated; // bit i: regions[i] is one mprotect call
};

#endif
