// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A board with no known status LED links this and the diagnostic LED does nothing.

#include <kickos/arch/arch.h>

extern "C" void arch_diag_led_set(int on)
{
    (void)on;
}
