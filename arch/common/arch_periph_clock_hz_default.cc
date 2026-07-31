// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// MUST return 0, never the core clock: a wrong branch clock silently garbles the wire,
// so 0 makes the querying driver use its explicit fallback.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C" uint32_t arch_periph_clock_hz(uintptr_t base)
{
    (void)base;
    return 0;
}
