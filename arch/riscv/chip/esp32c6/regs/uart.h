// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART0 registers (TRM v1.2 ch.26). The board console: UART0 (GPIO16/17)
// bridged to the host by the on-board CH343P. FIFO depth 128.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_UART_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_UART_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::uart
{
    constexpr uintptr_t FIFO = mmap::UART0_BASE + 0x00u;    // write = push a TX byte
    constexpr uintptr_t INT_ENA = mmap::UART0_BASE + 0x0Cu;
    constexpr uintptr_t INT_CLR = mmap::UART0_BASE + 0x10u;
    constexpr uintptr_t STATUS = mmap::UART0_BASE + 0x1Cu;  // TXFIFO_CNT [23:16]
    constexpr uintptr_t CONF1 = mmap::UART0_BASE + 0x24u;   // TXFIFO_EMPTY_THRHD [15:8]

    constexpr uint32_t TXFIFO_CNT_S = 16u;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t TXFIFO_LEN = 128u;
    constexpr uint32_t TXFIFO_LIMIT = TXFIFO_LEN - 2u;      // FIFO-full mark

    constexpr uint32_t TXFIFO_EMPTY_INT = 1u << 1;          // INT_ENA/INT_CLR bit1
    constexpr uint32_t TXFIFO_EMPTY_THRHD_S = 8u;
    constexpr uint32_t TXFIFO_EMPTY_THRHD_MASK = 0xFFu;
}

#endif
