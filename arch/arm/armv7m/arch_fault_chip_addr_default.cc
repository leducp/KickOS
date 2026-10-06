// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A chip whose isolation trap latches its address outside the core's MMFAR/BFAR (K64F
// SYSMPU) defines its own. See arch.h.

#include <kickos/arch/arch.h>

extern "C" bool arch_fault_chip_addr(uintptr_t*)
{
    return false;
}
