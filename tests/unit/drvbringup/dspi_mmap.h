// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K64F bases spi_dspi.cc and the bus table name, forced ahead of every include of
// drv_dspi_stall's sources: a host build generates no MK64F chip header, and both bases are host
// buffers here.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_DSPI_MMAP_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_DSPI_MMAP_H

#include <stdint.h>

namespace kickos::mk64f::mmap
{
    alignas(64) inline uint32_t g_dspi0_window[0x100u / 4u];
    alignas(64) inline uint32_t g_gpio_ports[0x140u / 4u]; // ports A to E
    inline uintptr_t const DSPI0_BASE = reinterpret_cast<uintptr_t>(g_dspi0_window);
    inline uintptr_t const GPIOA_BASE = reinterpret_cast<uintptr_t>(g_gpio_ports);
    constexpr uintptr_t GPIO_STRIDE = 0x40u;
}

#endif
