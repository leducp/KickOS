// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two bases <regs/usic.h> names, forced ahead of every include of drv_usic_route's sources:
// a host build generates no XMC4800 chip header. The engine is handed a host buffer instead.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_USIC_MMAP_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_USIC_MMAP_H

#include <stdint.h>

namespace kickos::xmc::mmap
{
    constexpr uintptr_t USIC0_CH0_BASE = 0x40030000u;
    constexpr uintptr_t USIC0_CH1_BASE = 0x40030200u;
}

#endif
