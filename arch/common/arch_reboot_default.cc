// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// A chip with no bootloader entry declines honestly. This symbol must be ABSENT from a
// production image, so arch/CMakeLists.txt adds this member only under
// KICKOS_ENABLE_SELFTEST, the same gate the callers and the chip backends carry.

#include <kickos/arch/arch.h>

#include <kickos/sys/errno.h>

extern "C" int arch_reboot(void)
{
    return -KOS_ENOSYS;
}
