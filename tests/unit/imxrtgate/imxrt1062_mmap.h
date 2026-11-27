// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The i.MX RT1062 bases <regs/aipstz.h> names (IMXRT1060RM Rev. 3, Tables 3-2 to 3-5 and 42.7).

#ifndef KICKOS_TESTS_UNIT_IMXRTGATE_IMXRT1062_MMAP_H
#define KICKOS_TESTS_UNIT_IMXRTGATE_IMXRT1062_MMAP_H

#include <stdint.h>

namespace kickos::imxrt1062::mmap
{
    constexpr uintptr_t USB1_BASE = 0x402E0000u;
    constexpr uintptr_t LPUART1_BASE = 0x40184000u;
    constexpr uintptr_t LPUART2_BASE = 0x40188000u;
    constexpr uintptr_t LPUART3_BASE = 0x4018C000u;
    constexpr uintptr_t LPUART4_BASE = 0x40190000u;
    constexpr uintptr_t LPUART5_BASE = 0x40194000u;
    constexpr uintptr_t LPUART6_BASE = 0x40198000u;
    constexpr uintptr_t LPUART7_BASE = 0x4019C000u;
    constexpr uintptr_t LPUART8_BASE = 0x401A0000u;
    constexpr uintptr_t AIPSTZ1_BASE = 0x4007C000u;
    constexpr uintptr_t AIPSTZ2_BASE = 0x4017C000u;
    constexpr uintptr_t AIPSTZ3_BASE = 0x4027C000u;
    constexpr uintptr_t AIPSTZ4_BASE = 0x4037C000u;
}

#endif
