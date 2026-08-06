// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 UART1 register map (RP2350 datasheet RP-008373-DS-2, 12.1): ARM PL011.
// The console is on UART1, not UART0 (the Pi-Zero header TX/RX pins land on
// GP4/GP5, which mux only UART1). Offsets are UART1_BASE-relative (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::uart
{
    constexpr uintptr_t DR = mmap::UART1_BASE + 0x00u;
    constexpr uintptr_t FR = mmap::UART1_BASE + 0x18u;
    constexpr uintptr_t IBRD = mmap::UART1_BASE + 0x24u;
    constexpr uintptr_t FBRD = mmap::UART1_BASE + 0x28u;
    constexpr uintptr_t LCR_H = mmap::UART1_BASE + 0x2cu;
    constexpr uintptr_t CR = mmap::UART1_BASE + 0x30u;
    constexpr uintptr_t IMSC = mmap::UART1_BASE + 0x38u; // interrupt mask set/clear

    constexpr uint32_t FR_TXFF = 1u << 5;   // TX (single holding location) full
    constexpr uint32_t IMSC_TXIM = 1u << 5; // transmit interrupt mask

    // baud = clk_peri / (16 x (IBRD + FBRD/64)), FBRD = round(frac x 64). clk_peri
    // 12 MHz, 115200 -> IBRD 6, FBRD 33; clk_peri 150 MHz -> IBRD 81, FBRD 24
    // (actual 115207 baud, +0.006%).
    constexpr uint32_t IBRD_115200 = 6u;
    constexpr uint32_t FBRD_115200 = 33u;
    constexpr uint32_t IBRD_150MHZ = 81u;
    constexpr uint32_t FBRD_150MHZ = 24u;

    // WLEN=8, no parity, one stop. FEN is deliberately LEFT OFF so the ring's
    // idle->busy prime starts the transfer regardless of level-vs-transition trigger.
    constexpr uint32_t LCR_H_8N1 = (0x3u << 5);                       // WLEN=8
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H
