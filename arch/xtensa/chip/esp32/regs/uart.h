// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 UART0 register map, from the ESP32 TRM v5.8 chapter 19 (UART Controller):
// offsets per section 19.4.1, fields per the per-register descriptions cited below.
// Console UART; the ROM leaves it at 115200 8N1 on GPIO1 TX / GPIO3 RX, which is also
// Register 19.9's reset framing (BIT_NUM=3, STOP_BIT_NUM=1, PARITY_EN=0).
//
// The RAW/ST/ENA/CLR quad behaves as the TRM appendix "Interrupt Configuration
// Registers" (Table 31.6-4) defines chip-wide:
//   ST == RAW & ENA, so INT_ST is the demux register: it already excludes disabled
//   sources and needs no software masking.
//   RAW is a LATCH, not a level follower. "When an interrupt source triggers, its RAW
//   bit is set to 1", and only writing 1 to the matching INT_CLR bit drops it.
// Two INT_CLR bits are additionally GATED on their cause already being gone (Register
// 19.5); written too early the clear does nothing and the source stays asserted:
//   RXFIFO_FULL_INT_CLR takes only while Rx_FIFO holds less than RXFIFO_FULL_THRHD.
//   RXFIFO_TOUT_INT_CLR takes only while rxfifo_cnt and rx_mem_cnt are both 0.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_UART_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_UART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::uart
{
    // TRM 19.4.1. OFF_* is the single source: a driver holding a granted window adds
    // them to its own base, the kernel uses the absolute aliases below.
    constexpr uintptr_t OFF_FIFO = 0x00u;    // [7:0] UART_RXFIFO_RD_BYTE; read = RX, write = TX
    constexpr uintptr_t OFF_INT_RAW = 0x04u; // RO
    constexpr uintptr_t OFF_INT_ST = 0x08u;  // RO
    constexpr uintptr_t OFF_INT_ENA = 0x0Cu; // R/W
    constexpr uintptr_t OFF_INT_CLR = 0x10u; // WO
    constexpr uintptr_t OFF_CLKDIV = 0x14u;
    constexpr uintptr_t OFF_STATUS = 0x1Cu; // RO
    constexpr uintptr_t OFF_CONF0 = 0x20u;
    constexpr uintptr_t OFF_CONF1 = 0x24u;

    constexpr uintptr_t FIFO = mmap::UART0_BASE + OFF_FIFO;
    constexpr uintptr_t INT_RAW = mmap::UART0_BASE + OFF_INT_RAW;
    constexpr uintptr_t INT_ST = mmap::UART0_BASE + OFF_INT_ST;
    constexpr uintptr_t INT_ENA = mmap::UART0_BASE + OFF_INT_ENA;
    constexpr uintptr_t INT_CLR = mmap::UART0_BASE + OFF_INT_CLR;
    constexpr uintptr_t CLKDIV = mmap::UART0_BASE + OFF_CLKDIV;
    constexpr uintptr_t STATUS = mmap::UART0_BASE + OFF_STATUS;
    constexpr uintptr_t CONF0 = mmap::UART0_BASE + OFF_CONF0;
    constexpr uintptr_t CONF1 = mmap::UART0_BASE + OFF_CONF1;

    // Interrupt bits, identical across RAW / ST / ENA / CLR (TRM Registers 19.2 to 19.5).
    constexpr uint32_t RXFIFO_FULL_INT = 1u << 0;
    constexpr uint32_t TXFIFO_EMPTY_INT = 1u << 1;
    constexpr uint32_t PARITY_ERR_INT = 1u << 2;
    constexpr uint32_t FRM_ERR_INT = 1u << 3;
    constexpr uint32_t RXFIFO_OVF_INT = 1u << 4;
    constexpr uint32_t RXFIFO_TOUT_INT = 1u << 8;
    constexpr uint32_t TX_DONE_INT = 1u << 14;

    // STATUS occupancy fields (TRM Register 19.8). Each is only the LOW 8 bits of its
    // count; the 3 high bits (tx_mem_cnt / rx_mem_cnt) sit in UART_MEM_CNT_STATUS_REG and
    // matter only once a FIFO is extended past its default 128-byte block.
    constexpr uint32_t TXFIFO_CNT_SHIFT = 16; // [23:16]
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t RXFIFO_CNT_SHIFT = 0; // [7:0]
    constexpr uint32_t RXFIFO_CNT_MASK = 0xFFu;

    // Transmitter finite state machine, same register (TRM Register 19.8), whose states the
    // TRM enumerates: 0 TX_IDLE, 1 TX_STRT, 2-9 TX_DAT0..7, 10 TX_PRTY, 11 TX_STP1,
    // 12 TX_STP2, 13 TX_DL0, 14 TX_DL1.
    constexpr uint32_t ST_UTX_OUT_SHIFT = 24; // [27:24]
    constexpr uint32_t ST_UTX_OUT_MASK = 0xFu;
    constexpr uint32_t ST_UTX_OUT_TX_IDLE = 0u;

    // CONF0 framing (TRM Register 19.9).
    constexpr uint32_t CONF0_PARITY = 1u << 0; // 0 even, 1 odd
    constexpr uint32_t CONF0_PARITY_EN = 1u << 1;
    constexpr uint32_t CONF0_BIT_NUM_SHIFT = 2; // 0:5b 1:6b 2:7b 3:8b
    constexpr uint32_t CONF0_BIT_NUM_MASK = 0x3u;
    constexpr uint32_t CONF0_STOP_BIT_NUM_SHIFT = 4; // 1:1b 2:1.5b 3:2b; 0 is invalid
    constexpr uint32_t CONF0_STOP_BIT_NUM_MASK = 0x3u;
    constexpr uint32_t CONF0_RXFIFO_RST = 1u << 17; // R/W, NOT self-clearing
    constexpr uint32_t CONF0_TXFIFO_RST = 1u << 18; // R/W, NOT self-clearing
    // Module clock select: 1 = APB_CLK, 0 = REF_TICK. CLKDIV below is computed against
    // the 80 MHz APB, so clearing this re-times every byte.
    constexpr uint32_t CONF0_TICK_REF_ALWAYS_ON = 1u << 27;

    // CONF1 thresholds (TRM Register 19.10), 7 bits each.
    constexpr uint32_t TXFIFO_EMPTY_THRHD_SHIFT = 8; // [14:8]
    constexpr uint32_t TXFIFO_EMPTY_THRHD_MASK = 0x7Fu;
    constexpr uint32_t RXFIFO_FULL_THRHD_SHIFT = 0; // [6:0]
    constexpr uint32_t RXFIFO_FULL_THRHD_MASK = 0x7Fu;

    // CLKDIV fractional divider (TRM Register 19.6): integer [19:0], 1/16 fraction [23:20].
    constexpr uint32_t CLKDIV_INT_MASK = 0xFFFFFu;
    constexpr uint32_t CLKDIV_FRAC_SHIFT = 20;

    // Default FIFO block per controller, out of the 1024-byte RAM the three UARTs share
    // (TRM 19.3.3, Figure 19.3-2).
    constexpr uint32_t FIFO_DEPTH = 128;

    // Software / config policy (not HW register-defined).
    constexpr uint32_t TXFIFO_LIMIT = 126;      // headroom below FIFO_DEPTH
    constexpr uint32_t TXFIFO_EMPTY_THRHD = 64; // chosen threshold value
    constexpr uint32_t RXFIFO_FULL_THRHD = 1;   // every byte raises, so a REPL sees one keystroke
    constexpr uint32_t CONSOLE_BAUD = 115200u;  // baud the ROM left UART0 at

    // 8N1 on the APB clock as one absolute CONF0 word. Storing it also clears TXD_INV,
    // LOOPBACK, IRDA_EN, TX_FLOW_EN and TXD_BRK, each of which silently corrupts or
    // withholds every outgoing byte.
    constexpr uint32_t CONF0_8N1 = CONF0_TICK_REF_ALWAYS_ON | (3u << CONF0_BIT_NUM_SHIFT)
                                   | (1u << CONF0_STOP_BIT_NUM_SHIFT);
}

#endif
