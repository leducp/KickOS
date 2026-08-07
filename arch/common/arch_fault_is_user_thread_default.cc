// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A backend that has not opted into fault isolation links this, and every fault on it
// panics. See arch.h.

#include <kickos/arch/arch.h>

extern "C" bool arch_fault_is_user_thread(void*)
{
    return false;
}
