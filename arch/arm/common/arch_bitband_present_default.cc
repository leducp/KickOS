// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Rule 7 bit-band flag (arch.h). The bit-band M4 chips (mk64f, stm32f411, xmc4800)
// answer 1 so the grant path also refuses a reserved block's alias image.

#include <kickos/arch/arch.h>

extern "C" int arch_bitband_present(void)
{
    return 0;
}
