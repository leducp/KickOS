// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bases the chip <regs/...> headers console_baud reads name, forced ahead of every include:
// a host build generates no chip header for these parts.

#ifndef KICKOS_TESTS_UNIT_CONSOLEBAUD_DIVISOR_MMAP_H
#define KICKOS_TESTS_UNIT_CONSOLEBAUD_DIVISOR_MMAP_H

#include <stdint.h>

namespace kickos::esp32::mmap
{
    constexpr uintptr_t UART0_BASE = 0x3FF40000u;
}

namespace kickos::rx::mmap
{
    constexpr uintptr_t SCI6 = 0x0008A0C0u;
}

namespace kickos::xmc::mmap
{
    constexpr uintptr_t USIC0_CH0_BASE = 0x40030000u;
    constexpr uintptr_t USIC0_CH1_BASE = 0x40030200u;
}

#endif
