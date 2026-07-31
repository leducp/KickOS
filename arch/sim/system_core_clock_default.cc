// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// CMSIS convention: the core clock in Hz, defined and maintained by the chip backend at
// PLL bring-up. The sim has no chip, so it needs this 0; a board that forgets it must
// fail the LINK, which is why this member is in the sim arch library only.
// Referenced by the microbench (kernel/bench/bench.cc); 0 disables the cycle->ns
// conversion, and the sim reports throughput only.

#include <stdint.h>

extern "C"
{
    uint32_t SystemCoreClock = 0;
}
