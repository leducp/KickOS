// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M RX-MPU: the RSPAGEn/REPAGEn pair a switch writes per region slot. REPAGE carries
// the access bits and the V bit, so a slot is deactivated by writing its REPAGE word 0.

#ifndef KICKOS_ARCH_MPU_ENCODED_H
#define KICKOS_ARCH_MPU_ENCODED_H

#include <stdint.h>

// Descriptor slots the image carries. kernel/include/kickos/mpuset.h static_asserts that
// KICKOS_MPU_MAX_REGIONS fits.
#define ARCH_MPU_ENCODED_SLOTS 8

struct arch_mpu_encoded
{
    uint32_t rspage[ARCH_MPU_ENCODED_SLOTS]; // region start page
    uint32_t repage[ARCH_MPU_ENCODED_SLOTS]; // region end page + UAC + V
};

#endif
