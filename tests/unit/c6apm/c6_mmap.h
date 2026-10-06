// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 bases <regs/apm.h> names (TRM v1.2 Table 5.3-2, p.176).

#ifndef KICKOS_TESTS_UNIT_C6APM_C6_MMAP_H
#define KICKOS_TESTS_UNIT_C6APM_C6_MMAP_H

#include <stdint.h>

namespace kickos::esp32c6::mmap
{
    constexpr uintptr_t HP_TEE_BASE = 0x60098000u;
    constexpr uintptr_t HP_APM_BASE = 0x60099000u;
    constexpr uintptr_t LP_APM_BASE = 0x600B3800u;
}

#endif
