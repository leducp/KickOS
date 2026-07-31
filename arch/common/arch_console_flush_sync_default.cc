// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A board whose console peripheral clock does not move with the core clock, or that
// cannot retune at all, needs no shift-register drain. See arch.h for the contract.

#include <kickos/arch/arch.h>

extern "C" void arch_console_flush_sync(void)
{
}
