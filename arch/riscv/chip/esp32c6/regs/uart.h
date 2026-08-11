// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART registers (TRM v1.2 ch.27, offsets from section 27.7.1). The board console
// is UART0 (GPIO16/17), bridged to the host by the on-board CH343P. Transmitter and receiver
// each own a 128 x 8-bit FIFO RAM (TRM section 27.4.2).

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
    constexpr uintptr_t OFF_INT_CLR = 0x10u;     // WT: write 1 drops the matching INT_RAW latch
    constexpr uintptr_t OFF_CLKDIV_SYNC = 0x14u; // baud divisor (Register 27.7)
    constexpr uintptr_t OFF_STATUS = 0x1Cu;      // RXFIFO_CNT [7:0], TXFIFO_CNT [23:16]
    constexpr uintptr_t OFF_CONF0_SYNC = 0x20u;  // framing (Register 27.9)
    constexpr uintptr_t OFF_CONF1 = 0x24u;       // RXFIFO_FULL_THRHD, TXFIFO_EMPTY_THRHD
    constexpr uintptr_t OFF_REG_UPDATE = 0x98u;  // Register 27.36; see REG_UPDATE_BIT below

    // A _SYNC register (TRM section 27.5.1) is read in the UART core's clock domain and
    // reaches it only on the OFF_REG_UPDATE write, so a store to one is inert on its own.
    constexpr uintptr_t OFF_CLKDIV = 0x14u;      // _SYNC: baud divisor, integral + /16 fraction
    constexpr uintptr_t OFF_CONF0 = 0x20u;       // _SYNC: framing, flow control, inversion, FIFO reset
    constexpr uintptr_t OFF_SWFC_CONF0 = 0x3Cu;  // _SYNC: XON/XOFF characters + FORCE_XOFF
    constexpr uintptr_t OFF_IDLE_CONF = 0x48u;   // _SYNC: RX_IDLE_THRHD [9:0], TX_IDLE_NUM [19:10]
    constexpr uintptr_t OFF_RS485_CONF = 0x4Cu;  // _SYNC: RS485 mode and its turnaround delays
    constexpr uintptr_t OFF_CLK_CONF = 0x88u;    // TX/RX core clock enable + core reset

    // UART0 absolute addresses, for the kernel console. A userspace driver uses OFF_* against
    // its granted window instead.
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
    constexpr uint32_t TXFIFO_LIMIT = TXFIFO_LEN - 2u;      // push stops here, 2 spare entries

    // INT_RAW / INT_ST / INT_ENA / INT_CLR share one bit layout (TRM Registers 27.3 - 27.6);
    // only the sub-sources KickOS routes are named. An INT_RAW bit is self-SET by its
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

    // CLKDIV_SYNC fractional divisor (TRM Register 27.7): integer [11:0], 1/16 fraction
    // [23:20]. The integer part is TWELVE bits here, not the 20 the earlier Espressif parts
    // gave it.
    constexpr uint32_t CLKDIV_INT_MASK = 0xFFFu;
    constexpr uint32_t CLKDIV_FRAG_S = 20u;
    constexpr uint32_t CLKDIV_FRAG_MASK = 0xFu;

    // CONF0_SYNC framing (TRM Register 27.9). STOP_BIT_NUM 0 is "Invalid. No effect", so a
    // zeroed field is not one stop bit.
    constexpr uint32_t CONF0_PARITY = 1u << 0; // 0 even, 1 odd
    constexpr uint32_t CONF0_PARITY_EN = 1u << 1;
    constexpr uint32_t CONF0_BIT_NUM_S = 2u; // 0:5b 1:6b 2:7b 3:8b
    constexpr uint32_t CONF0_BIT_NUM_MASK = 0x3u;
    constexpr uint32_t CONF0_STOP_BIT_NUM_S = 4u; // 1:1b 2:1.5b 3:2b
    constexpr uint32_t CONF0_STOP_BIT_NUM_MASK = 0x3u;

    // A write to a _SYNC register does nothing until this bit carries it across (TRM section
    // 27.5.1). It self-clears when the crossing completes, and section 27.5.2.2 wants the
    // wait on BOTH SIDES of a burst: reading it 0 first proves the PREVIOUS crossing
    // finished.
    constexpr uint32_t REG_UPDATE_BIT = 1u << 0;

    // --- Reclaim values (arch_console_reclaim). Each is the register's RESET value, which
    //     is also the polled-ready console configuration; the ROM leaves UART0 there.

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
}

#endif
