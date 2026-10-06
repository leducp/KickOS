// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V PMP (privileged spec 3.7.1): the lowest-numbered entry matching an address decides a
// U-mode access, and one no entry matches is refused.

#define arch_mpu_encode hw_rv32imac_encode_unused
#define arch_mpu_apply hw_rv32imac_apply_unused
#define arch_mpu_apply_now hw_rv32imac_apply_now_unused
#define arch_mpu_encoded hw_pmp_encoded
#define arch_mpu_encoded_seated hw_pmp_seated
#define arch_mpu_region_encodable hw_pmp_encodable

#include "arch/riscv/rv32imac/include/kickos/arch/mpu_encoded.h"
#include "hw.h"

#include "arch/riscv/rv32imac/pmp_encode.h"

extern "C" bool hw_pmp_encodable(uintptr_t base, size_t size)
{
    return hw_encodable(base, size);
}

HwAccess hw_rv32imac(arch_mpu_region const* regions, size_t n, uintptr_t addr, uint32_t* seated)
{
    hw_pmp_encoded img = {};
    *seated = kickos::rv32::pmp_encode(regions, n, &img);
    uint64_t below = 0;
    for (unsigned i = 0; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        uint32_t const cfg = (img.cfg[i / 4u] >> (8u * (i % 4u))) & 0xFFu;
        uint64_t const y = img.addr[i];
        uint64_t base = 0;
        uint64_t size = 0;
        uint32_t const a = (cfg >> 3) & 3u;
        if (a == 1u) // TOR
        {
            base = below << 2;
            if ((y << 2) > base)
            {
                size = (y << 2) - base;
            }
        }
        else if (a == 2u) // NA4
        {
            base = y << 2;
            size = 4u;
        }
        else if (a == 3u) // NAPOT
        {
            unsigned ones = 0;
            while (((y >> ones) & 1u) != 0)
            {
                ones++;
            }
            base = (y & ~((uint64_t(1) << (ones + 1u)) - 1u)) << 2;
            size = uint64_t(1) << (ones + 3u);
        }
        below = y;
        if (size != 0 and addr >= base and addr - base < size)
        {
            return {(cfg & 1u) != 0, (cfg & 2u) != 0};
        }
    }
    return {false, false};
}
