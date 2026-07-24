// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 UART0 register map (ESP32 TRM UART chapter). Console UART; the ROM
// leaves it at 115200 8N1 on GPIO1 TX / GPIO3 RX.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_UART_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_UART_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32::reg::uart
{
    constexpr uintptr_t FIFO = mmap::UART0_BASE + 0x00u;
    constexpr uintptr_t INT_ENA = mmap::UART0_BASE + 0x0Cu; // per-source interrupt enable
    constexpr uintptr_t INT_CLR = mmap::UART0_BASE + 0x10u; // write-1-to-clear
    constexpr uintptr_t CLKDIV = mmap::UART0_BASE + 0x14u;
    constexpr uintptr_t STATUS = mmap::UART0_BASE + 0x1Cu;
    constexpr uintptr_t CONF1 = mmap::UART0_BASE + 0x24u;

    // STATUS.TXFIFO_CNT [23:16]: bytes currently in the 128-entry TX FIFO.
    constexpr uint32_t TXFIFO_CNT_SHIFT = 16;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFFu;

    // INT bit 1 = TX FIFO below the empty threshold. A LEVEL source: it stays
    // asserted while txfifo_cnt < CONF1.TXFIFO_EMPTY_THRHD, so INT_ENA (not INT_CLR)
    // is what gates it.
    constexpr uint32_t TXFIFO_EMPTY_INT = 1u << 1;

    // CONF1.TXFIFO_EMPTY_THRHD: fire while the FIFO holds fewer than this many
    // bytes. Field width/position HW-unverified (review: "CONF1 THRHD width"); 7-bit
    // mask is conservative for the 128-deep FIFO.
    constexpr uint32_t TXFIFO_EMPTY_THRHD_SHIFT = 8;
    constexpr uint32_t TXFIFO_EMPTY_THRHD_MASK = 0x7Fu;

    // CLKDIV fractional divider: integer part [19:0], 1/16 fraction [23:20].
    constexpr uint32_t CLKDIV_INT_MASK = 0xFFFFFu;
    constexpr uint32_t CLKDIV_FRAC_SHIFT = 20;

    // Software / config policy (not HW register-defined).
    constexpr uint32_t TXFIFO_LIMIT = 126;        // headroom below the 128-deep FIFO
    constexpr uint32_t TXFIFO_EMPTY_THRHD = 64;    // chosen threshold value
    constexpr uint32_t CONSOLE_BAUD = 115200u;     // baud the ROM left UART0 at
}

#endif // KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_UART_H
