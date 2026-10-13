// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See region_seam.h.

#include "region_seam.h"

extern "C"
{
    uintptr_t arch_ram_base(void) { return g_region_seam.ram_base; }
    size_t arch_ram_size(void) { return g_region_seam.ram_size; }
    size_t arch_mpu_min_region(void) { return g_region_seam.min_region; }
    int arch_mpu_region_pow2(void) { return g_region_seam.pow2; }
    int arch_mpu_nocache_support(void) { return g_region_seam.nocache; }
    struct arch_reserved_span arch_reserved_blocks(void) { return g_region_seam.reserved; }
    struct arch_reserved_span arch_bus_master_apertures(void) { return g_region_seam.bus_masters; }
    int arch_bitband_present(void) { return g_region_seam.bitband; }

    bool arch_mpu_region_encodable(uintptr_t base, size_t size)
    {
        size_t const min = g_region_seam.min_region;
        if (size == 0 or size < min)
        {
            return false;
        }
        if (g_region_seam.pow2 != 0)
        {
            return (size & (size - 1u)) == 0u and (base & (size - 1u)) == 0u;
        }
        return min == 0 or (size % min == 0 and base % min == 0);
    }
}
