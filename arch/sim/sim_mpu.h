// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sim's MPU lowering, apart from the arena and mprotect so a host test can run it.

#ifndef KICKOS_ARCH_SIM_SIM_MPU_H
#define KICKOS_ARCH_SIM_SIM_MPU_H

#include <kickos/arch/arch.h>

#include <sys/mman.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace sim
    {
        inline int prot_from_attr(uint32_t attr)
        {
            int prot = PROT_NONE;
            if (attr & ARCH_MPU_R)
            {
                prot |= PROT_READ;
            }
            if (attr & ARCH_MPU_W)
            {
                prot |= PROT_WRITE;
            }
            if (attr & ARCH_MPU_X)
            {
                prot |= PROT_EXEC;
            }
            return prot;
        }

        // In slot order, never base order: under ARCH_MPU_OVERLAP_HIGHER the last call decides.
        template<class Valid, class Protect>
        void protect_in_slot_order(struct arch_mpu_region const* regions, size_t n, Valid valid,
                                   Protect protect)
        {
            for (size_t i = 0; i < n; i++)
            {
                if (valid(regions[i].base, regions[i].size))
                {
                    protect(regions[i].base, regions[i].size, prot_from_attr(regions[i].attr));
                }
            }
        }
    }
}

#endif
