// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 UART0 register map (RP2040 datasheet, RP-008371-DS, 4.2): an ARM PL011.
// Baud divisors latch only on the subsequent LCR_H write, so IBRD/FBRD must be
// written before LCR_H. Offsets are instance-relative to a UARTn base.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H

#include "../../rp2xxx/pl011_baud.h"
#include "clocks.h"

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::uart
{
    constexpr uintptr_t BASE = mmap::UART0_BASE;
    constexpr uintptr_t DR = mmap::UART0_BASE + 0x00u;
    constexpr uintptr_t FR = mmap::UART0_BASE + 0x18u;
    constexpr uintptr_t IBRD = mmap::UART0_BASE + 0x24u;
    constexpr uintptr_t FBRD = mmap::UART0_BASE + 0x28u;
    constexpr uintptr_t LCR_H = mmap::UART0_BASE + 0x2cu;
    constexpr uintptr_t CR = mmap::UART0_BASE + 0x30u;
    constexpr uintptr_t IFLS = mmap::UART0_BASE + 0x34u; // interrupt FIFO level select
    constexpr uintptr_t IMSC = mmap::UART0_BASE + 0x38u; // interrupt mask set/clear
    constexpr uintptr_t DMACR = mmap::UART0_BASE + 0x48u; // DMA control

    constexpr uint32_t FR_TXFF = 1u << 5;  // TX (single holding location) full
    // Covers the shift register too, and is set from the moment the TX FIFO goes
    // non-empty whether or not the UART is enabled (DS 4.2.8, UARTFR).
    constexpr uint32_t FR_BUSY = 1u << 3;
    constexpr uint32_t IMSC_TXIM = 1u << 5; // transmit interrupt mask

    // RXIFLSEL and TXIFLSEL both reset to b010 (DS 4.2.8, UARTIFLS).
    constexpr uint32_t IFLS_RESET = (0x2u << 3) | 0x2u;

    // WLEN=8, no parity, one stop. FEN (FIFO enable) is deliberately left OFF: with
    // the TX FIFO on, the PL011 transmit interrupt fires only as the FIFO descends
    // through the watermark, so a one-byte prime would never re-trigger the drain.
    constexpr uint32_t LCR_H_8N1 = (0x3u << 5); // WLEN=8
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE

    // clk_peri is the 12 MHz XOSC until clk_sys is on the PLL, then clk_sys.
    constexpr uint32_t IBRD_115200 = rp2xxx::pl011_ibrd(12000000u, 115200u);
    constexpr uint32_t FBRD_115200 = rp2xxx::pl011_fbrd(12000000u, 115200u);
    constexpr uint32_t IBRD_PLL = rp2xxx::pl011_ibrd(clocks::CLK_SYS_HZ, 115200u);
    constexpr uint32_t FBRD_PLL = rp2xxx::pl011_fbrd(clocks::CLK_SYS_HZ, 115200u);
}

#endif
