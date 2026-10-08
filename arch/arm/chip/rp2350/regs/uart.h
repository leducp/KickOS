// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 UART1 register map (RP2350 datasheet RP-008373-DS-2, 12.1): ARM PL011.
// The console is on UART1. Offsets are UART1_BASE-relative (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H

#include "../../rp2xxx/pl011_baud.h"
#include "clocks.h"
#include "xosc.h"

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::uart
{
    constexpr uintptr_t BASE = mmap::UART1_BASE;
    constexpr uintptr_t DR = mmap::UART1_BASE + 0x00u;
    constexpr uintptr_t FR = mmap::UART1_BASE + 0x18u;
    constexpr uintptr_t IBRD = mmap::UART1_BASE + 0x24u;
    constexpr uintptr_t FBRD = mmap::UART1_BASE + 0x28u;
    constexpr uintptr_t LCR_H = mmap::UART1_BASE + 0x2cu;
    constexpr uintptr_t CR = mmap::UART1_BASE + 0x30u;
    constexpr uintptr_t IFLS = mmap::UART1_BASE + 0x34u; // interrupt FIFO level select
    constexpr uintptr_t IMSC = mmap::UART1_BASE + 0x38u; // interrupt mask set/clear
    constexpr uintptr_t DMACR = mmap::UART1_BASE + 0x48u; // DMA control

    constexpr uint32_t FR_TXFF = 1u << 5;   // TX (single holding location) full
    // Covers the shift register too, and is set from the moment the TX FIFO goes
    // non-empty whether or not the UART is enabled (DS 12.1.8, UARTFR).
    constexpr uint32_t FR_BUSY = 1u << 3;
    constexpr uint32_t IMSC_TXIM = 1u << 5; // transmit interrupt mask

    // RXIFLSEL and TXIFLSEL both reset to b010 (DS 12.1.8, UARTIFLS).
    constexpr uint32_t IFLS_RESET = (0x2u << 3) | 0x2u;

    // clk_peri is the 12 MHz XOSC until clk_sys is on the PLL, then clk_sys.
    constexpr uint32_t IBRD_115200 = rp2xxx::pl011_ibrd(xosc::FREQ_HZ, 115200u);
    constexpr uint32_t FBRD_115200 = rp2xxx::pl011_fbrd(xosc::FREQ_HZ, 115200u);
    constexpr uint32_t IBRD_PLL = rp2xxx::pl011_ibrd(clocks::CLK_SYS_HZ, 115200u);
    constexpr uint32_t FBRD_PLL = rp2xxx::pl011_fbrd(clocks::CLK_SYS_HZ, 115200u);

    // WLEN=8, no parity, one stop. FEN is deliberately LEFT OFF so the ring's
    // idle->busy prime starts the transfer regardless of level-vs-transition trigger.
    constexpr uint32_t LCR_H_8N1 = (0x3u << 5);                       // WLEN=8
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE
}

#endif
