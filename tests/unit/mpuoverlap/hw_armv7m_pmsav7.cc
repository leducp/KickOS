// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PMSAv7 (ARMv7-M ARM B3.5): the highest-numbered enabled region matching an address decides
// it, and an address no region matches is privileged-only.

#undef KICKOS_ARM_MPU
#define KICKOS_ARM_MPU KICKOS_ARM_MPU_PMSAV7
#define arch_mpu_encode hw_pmsav7_encode
#define arch_mpu_apply hw_pmsav7_apply_unused
#define arch_mpu_apply_now hw_pmsav7_apply_now_unused
#define arch_mpu_encoded hw_pmsav7_encoded
#define arch_mpu_encoded_seated hw_pmsav7_seated
#define arch_mpu_region_encodable hw_pmsav7_encodable

#include "arch/arm/armv7m/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/arm/common/arch_mpu_encode_default.cc"

extern "C" bool hw_pmsav7_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

HwAccess hw_armv7m_pmsav7(arch_mpu_region const* regions, size_t n, uintptr_t addr,
                          uint32_t* seated)
{
    hw_pmsav7_encoded img = {};
    *seated = hw_pmsav7_encode(regions, n, &img);
    int ap = -1;
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        uint32_t const rasr = img.rasr[i];
        if ((rasr & 1u) == 0)
        {
            continue;
        }
        uint64_t const size = uint64_t(1) << (((rasr >> 1) & 0x1Fu) + 1u);
        uint64_t const base = img.rbar[i] & ~uint64_t(0x1F) & ~(size - 1u);
        if (addr < base or addr - base >= size)
        {
            continue;
        }
        if (size >= 256u and ((rasr >> (8u + (addr - base) / (size / 8u))) & 1u) != 0)
        {
            continue;
        }
        ap = static_cast<int>((rasr >> 24) & 7u);
    }
    // AP: 2 user read-only, 3 user read/write, 6 and 7 read-only; the rest user no access.
    HwAccess got = {false, false};
    if (ap == 2 or ap == 3 or ap == 6 or ap == 7)
    {
        got.read = true;
    }
    if (ap == 3)
    {
        got.write = true;
    }
    return got;
}
