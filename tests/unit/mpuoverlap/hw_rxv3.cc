// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M MPU (UM 17.1.4): an access is allowed when any valid region it hits allows it, and the
// background (MPBAC = 0) allows nothing.

#define arch_mpu_encode hw_rxv3_encode_unused
#define arch_mpu_apply hw_rxv3_apply_unused
#define arch_mpu_apply_now hw_rxv3_apply_now_unused
#define arch_mpu_encoded hw_rx_encoded
#define arch_mpu_encoded_seated hw_rx_seated
#define arch_mpu_region_encodable hw_rx_encodable

#include "arch/rx/rxv3/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/rx/rxv3/rx_mpu_encode.h"

extern "C" bool hw_rx_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

HwAccess hw_rxv3(arch_mpu_region const* regions, size_t n, uintptr_t addr, uint32_t* seated)
{
    hw_rx_encoded img = {};
    *seated = kickos::rxv3::rx_mpu_encode(regions, n, &img);
    uint32_t uac = 0;
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        if ((img.repage[i] & 1u) == 0)
        {
            continue;
        }
        uint64_t const start = img.rspage[i] & ~uint32_t(0xF);
        uint64_t const end = img.repage[i] | uint32_t(0xF);
        if (addr >= start and addr <= end)
        {
            uac |= img.repage[i] & 0xEu; // UAC: r 3, w 2, x 1
        }
    }
    return {(uac & 8u) != 0, (uac & 4u) != 0};
}
