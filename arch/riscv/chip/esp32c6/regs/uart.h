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

    // A _SYNC register (TRM section 27.5.1) is read in the UART core's clock domain and
    // reaches it only on the OFF_REG_UPDATE write, so a store to one is inert on its own.
    constexpr uintptr_t OFF_CLKDIV = 0x14u;      // _SYNC: baud divisor, integral + /16 fraction
    constexpr uintptr_t OFF_CONF0 = 0x20u;       // _SYNC: framing, flow control, inversion, FIFO reset
    constexpr uintptr_t OFF_SWFC_CONF0 = 0x3Cu;  // _SYNC: XON/XOFF characters + FORCE_XOFF
    constexpr uintptr_t OFF_IDLE_CONF = 0x48u;   // _SYNC: RX_IDLE_THRHD [9:0], TX_IDLE_NUM [19:10]
    constexpr uintptr_t OFF_RS485_CONF = 0x4Cu;  // _SYNC: RS485 mode and its turnaround delays
    constexpr uintptr_t OFF_CLK_CONF = 0x88u;    // TX/RX core clock enable + core reset
    constexpr uintptr_t OFF_REG_UPDATE = 0x98u;  // R/W/SC: write 1, hardware clears when synchronised

    // UART0 absolute addresses, for the kernel console. A userspace driver uses OFF_*
    // against its granted window instead.
    constexpr uintptr_t FIFO = mmap::UART0_BASE + OFF_FIFO;
    constexpr uintptr_t INT_RAW = mmap::UART0_BASE + OFF_INT_RAW;
    constexpr uintptr_t INT_ST = mmap::UART0_BASE + OFF_INT_ST;
    constexpr uintptr_t INT_ENA = mmap::UART0_BASE + OFF_INT_ENA;
    constexpr uintptr_t INT_CLR = mmap::UART0_BASE + OFF_INT_CLR;
    constexpr uintptr_t STATUS = mmap::UART0_BASE + OFF_STATUS;
    constexpr uintptr_t CONF1 = mmap::UART0_BASE + OFF_CONF1;
    constexpr uintptr_t CLKDIV = mmap::UART0_BASE + OFF_CLKDIV;
    constexpr uintptr_t CONF0 = mmap::UART0_BASE + OFF_CONF0;
    constexpr uintptr_t SWFC_CONF0 = mmap::UART0_BASE + OFF_SWFC_CONF0;
    constexpr uintptr_t IDLE_CONF = mmap::UART0_BASE + OFF_IDLE_CONF;
    constexpr uintptr_t RS485_CONF = mmap::UART0_BASE + OFF_RS485_CONF;
    constexpr uintptr_t CLK_CONF = mmap::UART0_BASE + OFF_CLK_CONF;
    constexpr uintptr_t REG_UPDATE = mmap::UART0_BASE + OFF_REG_UPDATE;

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

    // --- Reclaim values (arch_console_reclaim). Each is the register's RESET value, which
    //     is also the polled-ready console configuration; the ROM leaves UART0 there.

    // CONF0 (TRM Register 27.9): 8 data bits, 1 stop bit, no parity, and every inversion,
    // loopback, IrDA, break, flow-control and FIFO-reset bit clear.
    constexpr uint32_t CONF0_BIT_NUM_S = 2u;      // 3 = 8 data bits
    constexpr uint32_t CONF0_STOP_BIT_NUM_S = 4u; // 1 = one stop bit
    constexpr uint32_t CONF0_MEM_CLK_EN = 1u << 20;
    constexpr uint32_t CONF0_8N1 =
        (3u << CONF0_BIT_NUM_S) | (1u << CONF0_STOP_BIT_NUM_S) | CONF0_MEM_CLK_EN;

    // CLK_CONF (TRM Register 27.20): both core clocks running, neither core held in reset.
    constexpr uint32_t CLK_CONF_TX_SCLK_EN = 1u << 24;
    constexpr uint32_t CLK_CONF_RX_SCLK_EN = 1u << 25;
    constexpr uint32_t CLK_CONF_RUN = CLK_CONF_TX_SCLK_EN | CLK_CONF_RX_SCLK_EN;

    // SWFC_CONF0 (TRM Register 27.15): the XON/XOFF characters, with SW_FLOW_CON_EN and
    // FORCE_XOFF clear.
    constexpr uint32_t SWFC_CONF0_XON_CHAR = 0x11u;
    constexpr uint32_t SWFC_CONF0_XOFF_CHAR_S = 8u;
    constexpr uint32_t SWFC_CONF0_IDLE = (0x13u << SWFC_CONF0_XOFF_CHAR_S) | SWFC_CONF0_XON_CHAR;

    // IDLE_CONF (TRM Register 27.18): RX_IDLE_THRHD and TX_IDLE_NUM, both 0x100 bit times.
    constexpr uint32_t IDLE_CONF_TX_IDLE_NUM_S = 10u;
    constexpr uint32_t IDLE_CONF_DEFAULT = (0x100u << IDLE_CONF_TX_IDLE_NUM_S) | 0x100u;

    constexpr uint32_t REG_UPDATE_SYNC = 1u << 0;
}

#endif
