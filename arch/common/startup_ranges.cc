// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Generic C-runtime data init shared by chips that split app globals into their own
// pow2 MPU region: copy every {src, dst, len} triple in the linker's copy table
// (initialized .data-class sections, loaded from flash) then zero every {dst, len}
// pair in the zero table (.bss-class sections). One loop covers N ranges, so adding
// a range (kernel data, app data, a future per-domain block) is a linker-only edit
// with no startup churn. Runs from flash BEFORE any global is live, so it touches no
// global of its own -- the tables live in flash rodata, always readable at entry.
#include <stdint.h>

extern "C"
{
    extern uint32_t __kickos_copy_table_start[];
    extern uint32_t __kickos_copy_table_end[];
    extern uint32_t __kickos_zero_table_start[];
    extern uint32_t __kickos_zero_table_end[];

    void kickos_ranges_init(void)
    {
        for (uint32_t* e = __kickos_copy_table_start; e < __kickos_copy_table_end; e += 3)
        {
            uint32_t const* src = reinterpret_cast<uint32_t const*>(e[0]);
            uint32_t* dst = reinterpret_cast<uint32_t*>(e[1]);
            uint32_t const words = e[2] / 4u; // ranges are word-sized (linker ALIGN(4))
            for (uint32_t i = 0; i < words; i++)
            {
                dst[i] = src[i];
            }
        }
        for (uint32_t* e = __kickos_zero_table_start; e < __kickos_zero_table_end; e += 2)
        {
            uint32_t* dst = reinterpret_cast<uint32_t*>(e[0]);
            uint32_t const words = e[1] / 4u;
            for (uint32_t i = 0; i < words; i++)
            {
                dst[i] = 0;
            }
        }
    }
}
