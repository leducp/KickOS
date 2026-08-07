// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Unreachable: the core calls this only when arch_fault_is_user_thread returned true,
// and the fallback predicate never does. See arch.h.

#include <kickos/arch/arch.h>

extern "C" void arch_fault_redirect_to_exit(void*)
{
}
