// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// -KOS_ENOSYS, not 0: a driver whose block really is gated must fail loud instead of
// reading a block whose ungated registers BusFault.

#include <kickos/arch/arch.h>

#include <kickos/sys/errno.h>

#include <stdint.h>

extern "C" int arch_periph_enable(uintptr_t base)
{
    (void)base;
    return -KOS_ENOSYS;
}
