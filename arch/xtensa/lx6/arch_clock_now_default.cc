// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// CCOUNT with the software wrap extension. A chip that prefers a true 64-bit source
// (esp32 TIMG) defines its own: at 240 MHz CCOUNT wraps every ~17.9 s and a wrap not
// observed within one period is missed. The wrap state lives in arch_xtensa.cc (its
// bring-up resets it), so the read is reached through that TU.

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    uint64_t kickos_lx6_ccount_ns(void);

    uint64_t arch_clock_now(void)
    {
        return kickos_lx6_ccount_ns();
    }
}
