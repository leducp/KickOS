// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A controller that routes every line through the first-level ISR reserves none.

#include <kickos/arch/arch.h>

extern "C" bool arch_irq_line_kernel_owned(int line)
{
    (void)line;
    return false;
}
