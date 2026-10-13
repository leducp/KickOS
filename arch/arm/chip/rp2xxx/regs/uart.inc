// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PL011 both parts carry (RP2040 DS 4.2 / RP2350 DS 12.1), included inside a chip's
// reg::uart namespace after its BASE, with clocks.h, xosc.h and pl011_baud.h already included.
// Baud divisors latch only on the subsequent LCR_H write, so IBRD/FBRD are written before LCR_H.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_REGS_UART_INC
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_REGS_UART_INC

constexpr uintptr_t DR = BASE + 0x00u;
constexpr uintptr_t FR = BASE + 0x18u;
constexpr uintptr_t IBRD = BASE + 0x24u;
constexpr uintptr_t FBRD = BASE + 0x28u;
constexpr uintptr_t LCR_H = BASE + 0x2cu;
constexpr uintptr_t CR = BASE + 0x30u;
constexpr uintptr_t IFLS = BASE + 0x34u;  // interrupt FIFO level select
constexpr uintptr_t IMSC = BASE + 0x38u;  // interrupt mask set/clear
constexpr uintptr_t DMACR = BASE + 0x48u; // DMA control

constexpr uint32_t FR_TXFF = 1u << 5; // TX (single holding location) full
// Covers the shift register too, and is set from the moment the TX FIFO goes non-empty
// whether or not the UART is enabled (DS 4.2.8 / 12.1.8, UARTFR).
constexpr uint32_t FR_BUSY = 1u << 3;
constexpr uint32_t IMSC_TXIM = 1u << 5; // transmit interrupt mask

// RXIFLSEL and TXIFLSEL both reset to b010 (DS 4.2.8 / 12.1.8, UARTIFLS).
constexpr uint32_t IFLS_RESET = (0x2u << 3) | 0x2u;

// WLEN=8, no parity, one stop. FEN stays OFF: with the TX FIFO on, the transmit interrupt
// fires only as the FIFO descends through the watermark, so a one-byte prime would never
// re-trigger the drain.
constexpr uint32_t LCR_H_8N1 = (0x3u << 5);                       // WLEN=8
constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE

// clk_peri is the XOSC until clk_sys is on the PLL, then clk_sys.
constexpr uint32_t IBRD_115200 = rp2xxx::pl011_ibrd(xosc::FREQ_HZ, 115200u);
constexpr uint32_t FBRD_115200 = rp2xxx::pl011_fbrd(xosc::FREQ_HZ, 115200u);
constexpr uint32_t IBRD_PLL = rp2xxx::pl011_ibrd(clocks::CLK_SYS_HZ, 115200u);
constexpr uint32_t FBRD_PLL = rp2xxx::pl011_fbrd(clocks::CLK_SYS_HZ, 115200u);

#endif
