// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sim: the last mprotect over a page decides it, and the arena outside every region is
// PROT_NONE.

#include "hw.h"

#include "arch/sim/sim_mpu.h"

HwAccess hw_sim(arch_mpu_region const* regions, size_t n, uintptr_t addr, uint32_t* seated)
{
    *seated = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (hw_encodable(regions[i].base, regions[i].size))
        {
            *seated |= uint32_t(1) << i;
        }
    }
    int prot = PROT_NONE;
    kickos::sim::protect_in_slot_order(regions, n, hw_encodable,
                                       [addr, &prot](uintptr_t base, size_t size, int p) {
                                           if (addr >= base and addr - base < size)
                                           {
                                               prot = p;
                                           }
                                       });
    return {(prot & PROT_READ) != 0, (prot & PROT_WRITE) != 0};
}
