// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// No window: nothing to declare on a board that never hands the console to a userspace
// driver, nor on one whose console is not a memory-mapped device. The reclaim then runs on
// the last receiver's death, which is correct only while no OTHER thread can be holding
// the console's registers.

#include <kickos/arch/arch.h>

extern "C" void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = 0;
    *size = 0;
}
