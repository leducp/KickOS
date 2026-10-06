// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F SYSMPU (K64 RM 19.3): an access is allowed when any valid region descriptor it hits allows
// it. RGD0, which the kernel keeps, is taken to grant the core's user mode nothing.

#undef KICKOS_ARM_MPU
#define KICKOS_ARM_MPU KICKOS_ARM_MPU_SYSMPU
#define arch_mpu_encode hw_sysmpu_encode_unused
#define arch_mpu_apply hw_sysmpu_apply_unused
#define arch_mpu_apply_now hw_sysmpu_apply_now_unused
#define arch_mpu_encoded hw_sysmpu_encoded
#define arch_mpu_encoded_seated hw_sysmpu_seated
#define arch_mpu_region_encodable hw_sysmpu_encodable

#include "arch/arm/armv7m/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/arm/chip/mk64f/sysmpu_encode.h"

extern "C" bool hw_sysmpu_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

HwAccess hw_armv7m_sysmpu(arch_mpu_region const* regions, size_t n, uintptr_t addr,
                          uint32_t* seated)
{
    hw_sysmpu_encoded img = {};
    *seated = sysmpu_encode(regions, n, &img);
    uint32_t um = 0;
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        if (img.word2[i] == 0)
        {
            continue;
        }
        uint64_t const start = img.word0[i] & ~uint32_t(0x1F);
        uint64_t const end = img.word1[i] | uint32_t(0x1F);
        if (addr >= start and addr <= end)
        {
            um |= img.word2[i] & 7u; // M0UM: r 2, w 1, x 0
        }
    }
    return {(um & 4u) != 0, (um & 2u) != 0};
}
