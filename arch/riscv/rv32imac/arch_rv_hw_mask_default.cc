// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The software bitmask alone drops an injected raise, but a level-triggered device
// source keeps re-asserting until the controller is told to mask it, so a chip with a
// real device line must define this too or a mask-until-ack storms.

#include <kickos/arch/arch.h>

extern "C" void arch_rv_hw_mask(int line)
{
    (void)line;
}
