// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A chip whose isolation trap does not surface in the core CFSR (K64F SYSMPU) defines
// its own to add that capture. See arch.h.

#include <kickos/arch/arch.h>

extern "C" void arch_fault_report_extra(void)
{
}
