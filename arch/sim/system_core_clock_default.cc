// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// CMSIS convention: the core clock in Hz, defined and maintained by the chip backend at
// PLL bring-up. The sim has no chip, so it needs this 0; a board that forgets it must
// fail the LINK, which is why this member is in the sim arch library only. The rule reaches
// only the arches whose arch_cpu_clock_hz reads the symbol: armv6m/armv7m, rv32imac, rxv3
// and lx6. On armv8a, rv64imac and x86_64 nothing references it, so an omission there links.

#include <stdint.h>

extern "C"
{
    uint32_t SystemCoreClock = 0;
}
