// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 UART0 register map (RP2040 datasheet, RP-008371-DS, 4.2): an ARM PL011.
// Baud divisors latch only on the subsequent LCR_H write, so IBRD/FBRD must be
// written before LCR_H. Offsets are instance-relative to a UARTn base.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::uart
{
    constexpr uintptr_t DR = mmap::UART0_BASE + 0x00u;
    constexpr uintptr_t FR = mmap::UART0_BASE + 0x18u;
    constexpr uintptr_t IBRD = mmap::UART0_BASE + 0x24u;
    constexpr uintptr_t FBRD = mmap::UART0_BASE + 0x28u;
    constexpr uintptr_t LCR_H = mmap::UART0_BASE + 0x2cu;
    constexpr uintptr_t CR = mmap::UART0_BASE + 0x30u;
    constexpr uintptr_t IMSC = mmap::UART0_BASE + 0x38u; // interrupt mask set/clear

    constexpr uint32_t FR_TXFF = 1u << 5;  // TX (single holding location) full
    constexpr uint32_t IMSC_TXIM = 1u << 5; // transmit interrupt mask

    // WLEN=8, no parity, one stop. FEN (FIFO enable) is deliberately left OFF: with
    // the TX FIFO on, the PL011 transmit interrupt fires only as the FIFO descends
    // through the watermark, so a one-byte prime would never re-trigger the drain.
    constexpr uint32_t LCR_H_8N1 = (0x3u << 5); // WLEN=8
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE

    // baud = clk_peri / (16 x (IBRD + FBRD/64)), FBRD = round(frac x 64). clk_peri
    // 12 MHz, 115200 -> IBRD 6, FBRD 33; clk_peri 125 MHz (clk_sys on PLL) -> IBRD
    // 67, FBRD 52 (actual 115207 baud, +0.006%).
    constexpr uint32_t IBRD_115200 = 6u;
    constexpr uint32_t FBRD_115200 = 33u;
    constexpr uint32_t IBRD_125MHZ = 67u;
    constexpr uint32_t FBRD_125MHZ = 52u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H
