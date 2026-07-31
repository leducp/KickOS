// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// 1 == this core implements mcounteren (SiFive / QEMU virt). A chip whose core traps on
// `csrw mcounteren` (ESP32-C6 HP core) answers 0 so bring-up skips the write instead of
// taking an illegal-instruction trap into the not-yet-usable handler.

#include <kickos/arch/arch.h>

extern "C" int arch_rv_has_mcounteren(void)
{
    return 1;
}
