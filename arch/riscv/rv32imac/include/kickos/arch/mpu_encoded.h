// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32 PMP: the ten CSR words a switch writes. RV32 implements pmpaddr0..7 and
// packs the eight cfg BYTES four to a word across pmpcfg0/pmpcfg1, so the packing is a
// property of the whole set and not of any one region.

#ifndef KICKOS_ARCH_MPU_ENCODED_H
#define KICKOS_ARCH_MPU_ENCODED_H

#include <stdint.h>

// Descriptor slots the image carries. kernel/include/kickos/mpuset.h static_asserts that
// KICKOS_MPU_MAX_REGIONS fits.
#define ARCH_MPU_ENCODED_SLOTS 8

struct arch_mpu_encoded
{
    uint32_t addr[ARCH_MPU_ENCODED_SLOTS]; // pmpaddr0..7, NAPOT
    uint32_t cfg[2];                       // pmpcfg0, pmpcfg1
};

#endif
