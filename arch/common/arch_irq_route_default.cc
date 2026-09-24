// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A controller whose lines reach one core by construction, or one that routes by a
// mechanism the kernel is not told about, has no route to record.

#include <kickos/arch/arch.h>

extern "C" void arch_irq_route(int line, uint32_t core)
{
    (void)line;
    (void)core;
}
