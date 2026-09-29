// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// No per-core block apart from the kernel's own data, so there is nothing more to probe.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    uintptr_t arch_cpu_block_addr(void)
    {
        return 0;
    }
}
