// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// 0 == this chip cannot change its core clock at all. See arch.h for the contract.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C" uint32_t arch_cpu_clock_set(uint32_t target)
{
    (void)target;
    return 0;
}
