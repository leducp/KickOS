// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.

#include <kickos/arch/arch.h>

#include <kickos/sys/errno.h>

extern "C" int arch_amp_gate_probe(void)
{
    return -KOS_ENOSYS;
}
