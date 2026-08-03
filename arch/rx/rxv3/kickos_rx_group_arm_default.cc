// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A chip with no group interrupts has no line >= GROUP_LINE_BASE, so this is unreachable
// there rather than merely unused. Silent because arch_irq_mask/unmask are void; a
// masked-and-never-armed line simply never fires.

#include <kickos/arch/arch.h>

extern "C" void kickos_rx_group_arm(int line, int on)
{
    (void)line;
    (void)on;
}
