// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 LPUART register map (RM: Low Power UART), instanced for LPUART6
// (the Teensy 4.1 "Serial1" console). Register block is 32-bit access.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_LPUART_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_LPUART_H

#include <kickos/chip_mmap.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::imxrt1062::reg::lpuart
{
    // Register map, RM 49.6.1.1. The block is 0x30 bytes; LPUART6_WINDOW below is its whole
    // AIPS-2 slot, 4019_8000 to 4019_BFFF (RM Table 3-3).
    constexpr uintptr_t LPUART6_GLOBAL = mmap::LPUART6_BASE + 0x08u;
    constexpr uintptr_t LPUART6_BAUD = mmap::LPUART6_BASE + 0x10u;
    constexpr uintptr_t LPUART6_STAT = mmap::LPUART6_BASE + 0x14u;
    constexpr uintptr_t LPUART6_CTRL = mmap::LPUART6_BASE + 0x18u;
    constexpr uintptr_t LPUART6_DATA = mmap::LPUART6_BASE + 0x1Cu;
    constexpr size_t LPUART6_WINDOW = 0x4000u;

    constexpr uint32_t GLOBAL_RST = 1u << 1;  // module software reset
    constexpr uint32_t STAT_TDRE = 1u << 23;  // transmit-data-register empty
    // Transmission COMPLETE, RM 49.6.1.7: set only when the buffer is empty AND nothing is
    // being shifted out, so the pin has gone idle. TDRE above is only "the holding register
    // took it", so a flush that waits on TDRE truncates the last character.
    constexpr uint32_t STAT_TC = 1u << 22;
    constexpr uint32_t CTRL_TIE = 1u << 23;   // transmit-interrupt enable
    constexpr uint32_t CTRL_TE = 1u << 19;    // transmitter enable
    constexpr uint32_t CTRL_RE = 1u << 18;    // receiver enable

    // LPUART clock root: 20 MHz. clock_init() is deferred, so KickOS inherits the
    // boot-ROM CCM tree. RM Table 9-7 "ROM Clock Setting" sets CCM_CSCDR1 =
    // 0x06490B03, i.e. UART_CLK_SEL=0 (bit 6 -> pll3_80m) and UART_CLK_PODF=0b000011
    // = divide-by-4 (RM 14.7.9). With PLL_USB1 up (RM Table 9-7 CCM_ANALOG_PLL_USB1
    // = 0x8000_3040 -> 480 MHz), pll3_80m = 480/6 = 80 MHz, so uart_clk_root =
    // 80/4 = 20 MHz (clock tree RM Fig 14-3). NOT the reset-default 80 MHz (PODF
    // ignored) nor 24 MHz (wrong mux); both earlier guesses gave garbage baud.
    constexpr uint32_t UART_CLK_ROOT_HZ = 20000000u;

    // BAUD = (OSR << 24) | SBR, RM 49.6.1.6. baud = uart_clk / ((OSR + 1) * SBR).
    // ROUND TO NEAREST, never truncate: at the 20 MHz root the ideal divisor is 10.85, and
    // truncating to 10 gives 125000 baud (+8.5%), past receiver tolerance; nearest gives 11
    // = 113636 baud (-1.36%), inside it.
    constexpr uint32_t CONSOLE_BAUD = 115200u;
    constexpr uint32_t CONSOLE_OSR = 15u; // 16x oversample
    constexpr uint32_t baud_sbr(uint32_t hz, uint32_t baud, uint32_t osr)
    {
        uint32_t const div = baud * (osr + 1u);
        uint32_t const sbr = (hz + (div / 2u)) / div;
        if (sbr == 0u)
        {
            return 1u;
        }
        return sbr;
    }
    constexpr uint32_t BAUD_CONSOLE =
        (CONSOLE_OSR << 24)
        | (baud_sbr(UART_CLK_ROOT_HZ, CONSOLE_BAUD, CONSOLE_OSR) & 0x1FFFu);
}

#endif
