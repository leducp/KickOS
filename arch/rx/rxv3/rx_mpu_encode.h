// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RX MPU page registers for a region set, apart from the register writes so a host test can
// run the encoder the commit programs from.

#ifndef KICKOS_ARCH_RX_RXV3_RX_MPU_ENCODE_H
#define KICKOS_ARCH_RX_RXV3_RX_MPU_ENCODE_H

#include <kickos/arch/arch.h>

#include "regs.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU
namespace kickos
{
    namespace rxv3
    {
        // The page masks are field encoding, not rounding: a region the 16-byte pages cannot
        // represent EXACTLY gets REPAGE 0 (V clear), never a window widened by up to 15 bytes
        // on each side.
        inline uint32_t rx_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                                      struct arch_mpu_encoded* out)
        {
            if (n > ARCH_MPU_ENCODED_SLOTS)
            {
                n = ARCH_MPU_ENCODED_SLOTS;
            }
            uint32_t seated = 0;
            size_t i = 0;
            for (; i < n; i++)
            {
                out->rspage[i] = 0;
                out->repage[i] = 0;
                if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
                {
                    uintptr_t const base = regions[i].base;
                    uintptr_t const end = base + regions[i].size - 1; // inclusive last byte
                    uint32_t uac = 0;
                    if (regions[i].attr & ARCH_MPU_R)
                    {
                        uac |= MPU_UAC_R;
                    }
                    if (regions[i].attr & ARCH_MPU_W)
                    {
                        uac |= MPU_UAC_W;
                    }
                    if (regions[i].attr & ARCH_MPU_X)
                    {
                        uac |= MPU_UAC_X;
                    }
                    out->rspage[i] = static_cast<uint32_t>(base) & MPU_PAGE_MASK;
                    out->repage[i] = (static_cast<uint32_t>(end) & MPU_PAGE_MASK) | uac | MPU_REPAGE_V;
                    seated |= static_cast<uint32_t>(1) << i;
                }
            }
            for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
            {
                out->rspage[i] = 0;
                out->repage[i] = 0;
            }
            return seated;
        }
    }
}
#endif

#endif
