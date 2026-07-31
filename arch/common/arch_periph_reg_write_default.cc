// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// This chip has no privileged-write allowlist. -KOS_ENOSYS, not 0: a caller reaching
// for this seam has a register the bus refuses it, so a success answer would report a
// store that never happened. See arch.h for the contract.

#include <kickos/arch/arch.h>

#include <kickos/sys/errno.h>

#include <stdint.h>

extern "C" int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
{
    (void)base;
    (void)offset;
    (void)value;
    return -KOS_ENOSYS;
}
