// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PMSAv8 (ARMv8-M ARM, MPU region matching): an address two enabled regions match faults, and
// one no region matches is privileged-only.

#undef KICKOS_ARM_MPU
#define KICKOS_ARM_MPU KICKOS_ARM_MPU_PMSAV8
#define arch_mpu_encode hw_pmsav8_encode_unused
#define arch_mpu_apply hw_pmsav8_apply_unused
#define arch_mpu_apply_now hw_pmsav8_apply_now_unused
#define arch_mpu_encoded hw_pmsav8_encoded
#define arch_mpu_encoded_seated hw_pmsav8_seated
#define arch_mpu_region_encodable hw_pmsav8_encodable

#include "arch/arm/armv7m/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/arm/common/pmsav8_encode.h"

extern "C" bool hw_pmsav8_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

HwAccess hw_armv7m_pmsav8(arch_mpu_region const* regions, size_t n, uintptr_t addr,
                          uint32_t* seated)
{
    hw_pmsav8_encoded img = {};
    *seated = kickos::arm::pmsav8_encode(regions, n, &img);
    unsigned matches = 0;
    uint32_t ap = 0;
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        if ((img.rlar[i] & 1u) == 0)
        {
            continue;
        }
        uint64_t const base = img.rbar[i] & ~uint32_t(0x1F);
        uint64_t const limit = img.rlar[i] | uint32_t(0x1F);
        if (addr >= base and addr <= limit)
        {
            matches++;
            ap = (img.rbar[i] >> 1) & 3u;
        }
    }
    // AP: 1 read/write at any privilege, 3 read-only at any privilege; 0 and 2 privileged-only.
    HwAccess got = {false, false};
    if (matches == 1 and (ap == 1 or ap == 3))
    {
        got.read = true;
        got.write = (ap == 1);
    }
    return got;
}
