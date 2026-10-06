// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PMSAv8 descriptor words for a region set, apart from the register writes so a host test
// can run the encoder the commit programs from.

#ifndef KICKOS_ARCH_ARM_COMMON_PMSAV8_ENCODE_H
#define KICKOS_ARCH_ARM_COMMON_PMSAV8_ENCODE_H

#include <kickos/arch/arch.h>

#include "regs_v8m.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_HAVE_MPU
namespace kickos
{
    namespace arm
    {
        // The MPU_RBAR low attribute bits (SH|AP|XN). attr is the UNPRIVILEGED access;
        // supervisor comes from the PRIVDEFENA background. Code is RO+executable, data/stack
        // RW+execute-never, and a read-only data region RO-any.
        inline uint32_t pmsav8_rbar_attr(uint32_t attr)
        {
            if (attr & ARCH_MPU_X)
            {
                return RBAR_AP_RO_ANY; // code: RO-any, executable (XN=0), SH=0
            }
            uint32_t v = RBAR_XN; // data / MMIO: execute-never
            if (attr & ARCH_MPU_W)
            {
                v |= RBAR_AP_RW_ANY;
            }
            else
            {
                v |= RBAR_AP_RO_ANY;
            }
            return v;
        }

        // The MPU_RLAR AttrIndx bits, i.e. the MAIR0 slot kickos_arm_pmsav8_init programs.
        inline uint32_t pmsav8_rlar_attr(uint32_t attr)
        {
            if (attr & ARCH_MPU_DEV)
            {
                return RLAR_ATTR_DEVICE;
            }
            if (attr & ARCH_MPU_NOCACHE)
            {
                return RLAR_ATTR_NORMAL_NC;
            }
            return RLAR_ATTR_NORMAL;
        }

        // RBAR masks the base to a 32-byte boundary and RLAR the limit, so a region PMSAv8
        // cannot name exactly gets RLAR 0 (EN=0) rather than a window rounded outward from
        // what was asked.
        inline uint32_t pmsav8_encode(struct arch_mpu_region const* regions, size_t n,
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
                out->rbar[i] = 0;
                out->rlar[i] = 0;
                if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
                {
                    uintptr_t const base = regions[i].base;
                    uintptr_t const limit = base + regions[i].size - 1; // inclusive top
                    out->rbar[i] = (static_cast<uint32_t>(base) & RBAR_BASE_MASK)
                        | pmsav8_rbar_attr(regions[i].attr);
                    out->rlar[i] = (static_cast<uint32_t>(limit) & RLAR_LIMIT_MASK)
                        | pmsav8_rlar_attr(regions[i].attr) | RLAR_EN;
                    seated |= static_cast<uint32_t>(1) << i;
                }
            }
            for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
            {
                out->rbar[i] = 0;
                out->rlar[i] = 0;
            }
            return seated;
        }
    }
}

#endif

#endif
