// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// -KOS_ENOSYS, not 0: a bare "applied" would make a non-empty board pin-map a silent
// no-op success.

#include <kickos/arch/arch.h>

#include <kickos/sys/errno.h>

#include <stdint.h>

extern "C" int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    (void)port;
    (void)pin;
    (void)func;
    return -KOS_ENOSYS;
}
