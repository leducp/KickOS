// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 base <regs/accessctrl.h> names (RP2350 datasheet, Table 13, p.32).

#ifndef KICKOS_TESTS_UNIT_RP2350GATE_RP2350_MMAP_H
#define KICKOS_TESTS_UNIT_RP2350GATE_RP2350_MMAP_H

#include <stdint.h>

namespace kickos::rp2350::mmap
{
    constexpr uintptr_t ACCESSCTRL_BASE = 0x40060000u;
}

#endif
