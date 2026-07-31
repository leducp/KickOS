// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A board that never hands the console to a userspace driver needs no reclaim. A chip
// body must be idempotent straight-line register STORES: kpanic_enter can reach it
// from a partial nested-fault state.

#include <kickos/arch/arch.h>

extern "C" void arch_console_reclaim(void)
{
}
