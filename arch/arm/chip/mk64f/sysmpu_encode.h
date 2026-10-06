// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SYSMPU region descriptor words for a region set, apart from the register writes so a host
// test can run the encoder the commit programs from.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_ENCODE_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_SYSMPU_ENCODE_H

#include <kickos/arch/arch.h>

#include "sysmpu_rights.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU
// SRTADDR/ENDADDR are addr[31:5], so a region whose base or end is not 32-byte aligned would be
// programmed as a window rounded outward from what was asked; such a region gets WORD2 0
// instead, which the commit reads as "invalidate this descriptor".
static inline uint32_t sysmpu_encode(struct arch_mpu_region const* regions, size_t n,
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
        out->word0[i] = 0;
        out->word1[i] = 0;
        out->word2[i] = 0;
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            uintptr_t const base = regions[i].base;
            out->word0[i] = static_cast<uint32_t>(base);
            out->word1[i] = static_cast<uint32_t>(base + regions[i].size - 1);
            out->word2[i] = sysmpu_word2(regions[i].attr);
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->word0[i] = 0;
        out->word1[i] = 0;
        out->word2[i] = 0;
    }
    return seated;
}

#endif

#endif
