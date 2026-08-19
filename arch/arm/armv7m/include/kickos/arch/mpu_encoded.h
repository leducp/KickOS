// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The v7-M arch carries three MPU designs, because a chip may replace the core PMSAv7
// backend (arch/arm/chip/<chip>/mpu.cmake): PMSAv8 on a Cortex-M33 and the K64F's
// crossbar SYSMPU. Each writes a different descriptor, so each gets its own image.
// KICKOS_ARM_MPU (CMakeLists.txt) names which one this build is; this header is only
// included where KICKOS_HAVE_MPU (arch/include/kickos/arch/arch.h), so it never sees
// KICKOS_ARM_MPU_NONE.
//
// The slot index is positional in all three, so the hardware region number comes from the
// image index and is not carried in the words.

#ifndef KICKOS_ARCH_MPU_ENCODED_H
#define KICKOS_ARCH_MPU_ENCODED_H

#include <stdint.h>

// Descriptor slots the image carries. kernel/include/kickos/mpuset.h static_asserts that
// KICKOS_MPU_MAX_REGIONS fits.
#define ARCH_MPU_ENCODED_SLOTS 8

#if KICKOS_ARM_MPU == KICKOS_ARM_MPU_SYSMPU

struct arch_mpu_encoded
{
    uint32_t word0[ARCH_MPU_ENCODED_SLOTS]; // SRTADDR
    uint32_t word1[ARCH_MPU_ENCODED_SLOTS]; // ENDADDR
    uint32_t word2[ARCH_MPU_ENCODED_SLOTS]; // master access rights, 0 deactivates the slot
};

#elif KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV8

struct arch_mpu_encoded
{
    uint32_t rbar[ARCH_MPU_ENCODED_SLOTS];
    uint32_t rlar[ARCH_MPU_ENCODED_SLOTS]; // 0 deactivates the slot
};

#elif KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV7

// The core PMSAv7 backend in arch/arm/common/ programs from this layout.
struct arch_mpu_encoded
{
    uint32_t rbar[ARCH_MPU_ENCODED_SLOTS];
    uint32_t rasr[ARCH_MPU_ENCODED_SLOTS]; // 0 deactivates the slot
};

#else

#error "KICKOS_ARM_MPU names no backend"

#endif

#endif
