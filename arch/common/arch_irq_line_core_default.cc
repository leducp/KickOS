// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A controller that raises every line on every core, or one that routes by a mechanism the
// kernel is not told about, has no core to name.

#include <kickos/arch/arch.h>

extern "C" int arch_irq_line_core(int line)
{
    (void)line;
    return KICKOS_IRQ_LINE_CORE_NONE;
}
