// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 (Cortex-M33) peripheral base addresses (RP2350 datasheet
// RP-008373-DS-2). Bases only; register offsets + fields live in regs/<periph>.h.
// Hand-rolled clean-room, no vendor SDK. All APB peripheral bases moved relative
// to the RP2040 (datasheet 2.2.4).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_MMAP_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_MMAP_H

#include <stdint.h>

namespace kickos::rp2350::mmap
{
    constexpr uintptr_t CLOCKS_BASE = 0x40010000u;     // clock generators (DS 8.1)
    constexpr uintptr_t RESETS_BASE = 0x40020000u;     // peripheral reset control (DS 7.5)
    constexpr uintptr_t IO_BANK0_BASE = 0x40028000u;   // GPIO function select (DS 9.11.1)
    constexpr uintptr_t PADS_BANK0_BASE = 0x40038000u; // pad control (DS 9.11.3)
    constexpr uintptr_t XOSC_BASE = 0x40048000u;       // 12 MHz crystal oscillator (DS 8.2)
    constexpr uintptr_t PLL_SYS_BASE = 0x40050000u;    // system PLL (DS 8.6)
    constexpr uintptr_t UART1_BASE = 0x40078000u;      // ARM PL011, console (DS 12.1)
    constexpr uintptr_t TIMER0_BASE = 0x400B0000u;     // 64-bit us monotonic counter (DS 12.8)
    constexpr uintptr_t TICKS_BASE = 0x40108000u;      // common tick generators (DS 8.5)

    // Atomic register-access aliases (DS 2.1.3): every APB peripheral register is
    // mirrored at base+alias so a single-bit change needs no read-modify-write.
    // XOR (0x1000) completes the pattern; the chip backend uses only SET/CLR.
    constexpr uintptr_t ATOMIC_XOR = 0x1000u;
    constexpr uintptr_t ATOMIC_SET = 0x2000u;
    constexpr uintptr_t ATOMIC_CLR = 0x3000u;

    // Full window (base .. base+0x3FFF) covering the SET/CLR/XOR aliases; the size
    // used for the PMSAv8 reserved-block MPU grants (arch_reserved_blocks).
    constexpr uintptr_t APB_ATOMIC_WINDOW = 0x4000u;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2350_MMAP_H
