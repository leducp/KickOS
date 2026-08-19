// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M PMSA: the RBAR/RASR pair a switch writes per region slot. The slot index is
// positional, so the hardware region number comes from the image index plus the chip's
// fixed-region count and is not carried in RBAR.

#ifndef KICKOS_ARCH_MPU_ENCODED_H
#define KICKOS_ARCH_MPU_ENCODED_H

#include <stdint.h>

// Descriptor slots the image carries. kernel/include/kickos/mpuset.h static_asserts that
// KICKOS_MPU_MAX_REGIONS fits.
#define ARCH_MPU_ENCODED_SLOTS 8

// PMSAv8 is an ARMv8-M feature and the K64F SYSMPU is a chip that only ships under
// armv7m, so KICKOS_ARM_MPU cannot resolve to either on this arch; PMSAv7 is the only
// backend v6-M can reach. This header is only included where KICKOS_HAVE_MPU (see
// arch/include/kickos/arch/arch.h), so it never sees KICKOS_ARM_MPU_NONE either.
#if KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV7

struct arch_mpu_encoded
{
    uint32_t rbar[ARCH_MPU_ENCODED_SLOTS];
    uint32_t rasr[ARCH_MPU_ENCODED_SLOTS]; // 0 deactivates the slot
};

#else

#error "KICKOS_ARM_MPU names no backend v6-M can reach"

#endif

#endif
