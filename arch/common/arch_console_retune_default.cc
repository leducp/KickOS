// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A board whose UART baud does not track the core clock needs no re-derivation after
// a clock move. See arch.h for the contract.

#include <kickos/arch/arch.h>

extern "C" void arch_console_retune(void)
{
}
