// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Software-injected lines (the doorbell) need no per-line HW routing. A chip with a real
// device line (C6 UART0 TX ring) defines its own. Called INSIDE the arch critical
// section, so a controller reconfigure cannot glitch in a transient state.

#include <kickos/arch/arch.h>

extern "C" void arch_rv_hw_unmask(int line)
{
    (void)line;
}
