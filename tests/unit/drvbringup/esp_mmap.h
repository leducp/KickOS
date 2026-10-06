// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The base each ESP <regs/uart.h> names, forced ahead of drv_esp_uart_flush's sources: a host
// build generates no ESP chip header. The family body is handed a host buffer instead.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_ESP_MMAP_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_ESP_MMAP_H

#include <stdint.h>

namespace kickos::esp32::mmap
{
    constexpr uintptr_t UART0_BASE = 0x3FF40000u;
}

namespace kickos::esp32c6::mmap
{
    constexpr uintptr_t UART0_BASE = 0x60000000u;
}

#endif
