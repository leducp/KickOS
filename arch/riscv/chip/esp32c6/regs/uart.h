// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART registers (TRM v1.2 ch.27, offsets from section 27.7.1). The board
// console is UART0 (GPIO16/17), bridged to the host by the on-board CH343P. Transmitter
// and receiver each own a 128 x 8-bit FIFO RAM (TRM section 27.4.2).
//
// The offsets are exposed separately from the UART0 absolute addresses so an unprivileged
// driver applies them to the register window it was GRANTED, not to a base compiled in.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_UART_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_UART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::uart
{
    constexpr uintptr_t OFF_FIFO = 0x00u;    // [7:0] RXFIFO_RD_BYTE; a write pushes a TX byte
    constexpr uintptr_t OFF_INT_RAW = 0x04u; // R/WTC/SS: a LATCH, see INT_CLR below
    constexpr uintptr_t OFF_INT_ST = 0x08u;  // INT_RAW & INT_ENA, read-only
    constexpr uintptr_t OFF_INT_ENA = 0x0Cu;
    constexpr uintptr_t OFF_INT_CLR = 0x10u; // WT: write 1 to drop the matching INT_RAW latch
    constexpr uintptr_t OFF_STATUS = 0x1Cu;  // RXFIFO_CNT [7:0], TXFIFO_CNT [23:16]
    constexpr uintptr_t OFF_CONF1 = 0x24u;   // RXFIFO_FULL_THRHD [7:0], TXFIFO_EMPTY_THRHD [15:8]

    // UART0 absolute addresses, for the kernel console. A userspace driver uses OFF_*
    // against its granted window instead.
    constexpr uintptr_t FIFO = mmap::UART0_BASE + OFF_FIFO;
    constexpr uintptr_t INT_RAW = mmap::UART0_BASE + OFF_INT_RAW;
    constexpr uintptr_t INT_ST = mmap::UART0_BASE + OFF_INT_ST;
    constexpr uintptr_t INT_ENA = mmap::UART0_BASE + OFF_INT_ENA;
    constexpr uintptr_t INT_CLR = mmap::UART0_BASE + OFF_INT_CLR;
    constexpr uintptr_t STATUS = mmap::UART0_BASE + OFF_STATUS;
    constexpr uintptr_t CONF1 = mmap::UART0_BASE + OFF_CONF1;

    constexpr uint32_t RXFIFO_CNT_S = 0u;
    constexpr uint32_t RXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t TXFIFO_CNT_S = 16u;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t TXFIFO_LEN = 128u;
    constexpr uint32_t TXFIFO_LIMIT = TXFIFO_LEN - 2u;      // FIFO-full mark

    // INT_RAW / INT_ST / INT_ENA / INT_CLR share one bit layout (TRM Registers 27.3 - 27.6).
    // Only the sub-sources KickOS routes are named. An INT_RAW bit is self-SET by its
    // condition and cleared ONLY by writing 1 to the matching INT_CLR bit, so it stays
    // asserted after a level condition has gone away.
    constexpr uint32_t RXFIFO_FULL_INT = 1u << 0;
    constexpr uint32_t TXFIFO_EMPTY_INT = 1u << 1;
    constexpr uint32_t PARITY_ERR_INT = 1u << 2;
    constexpr uint32_t FRM_ERR_INT = 1u << 3;
    constexpr uint32_t RXFIFO_OVF_INT = 1u << 4;
    constexpr uint32_t RXFIFO_TOUT_INT = 1u << 8;

    constexpr uint32_t RXFIFO_FULL_THRHD_S = 0u;
    constexpr uint32_t RXFIFO_FULL_THRHD_MASK = 0xFFu;
    constexpr uint32_t TXFIFO_EMPTY_THRHD_S = 8u;
    constexpr uint32_t TXFIFO_EMPTY_THRHD_MASK = 0xFFu;
}

#endif
