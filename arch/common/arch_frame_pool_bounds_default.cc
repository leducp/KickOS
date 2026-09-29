// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The chip linker script's carve. A HAS_ASPACE chip that carves no pool and answers no
// bounds of its own fails the link on these two names.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    extern unsigned char __kickos_frame_pool_start[];
    extern unsigned char __kickos_frame_pool_end[];

    void arch_frame_pool_bounds(uintptr_t* base, uintptr_t* top)
    {
        *base = reinterpret_cast<uintptr_t>(__kickos_frame_pool_start);
        *top = reinterpret_cast<uintptr_t>(__kickos_frame_pool_end);
    }
}
