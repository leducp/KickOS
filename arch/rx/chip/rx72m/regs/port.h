// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M I/O-port register offsets + fields for the SCI6 console pins (PORTB) and the
// diag LED (PORT8). From the RX72M Group User's Manual: Hardware (r01uh0804ej0120,
// Rev.1.20) sec.22; hand-rolled, clean-room. Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_PORT_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_PORT_H

#include <stdint.h>

#include "../mmap.h"

namespace kickos::rx::reg::port
{
    // Register blocks (one byte per port): PORTn.<reg> = block + n (UM sec.22.3).
    constexpr uintptr_t PDR_BASE = mmap::PORT + 0x00;  // direction
    constexpr uintptr_t PODR_BASE = mmap::PORT + 0x20; // output data
    constexpr uintptr_t PIDR_BASE = mmap::PORT + 0x40; // pin state (readable whatever PDR/PMR hold)
    constexpr uintptr_t PMR_BASE = mmap::PORT + 0x60;  // mode (peripheral vs GPIO)

    // Port index runs 0..9, A..H, J..N, Q as a DENSE 0..0x17 (PORTG is 0x10, PORTQ is
    // 0x17): the letters skip I, O and P, the addresses do not (UM sec.22.3 lists).
    constexpr uint32_t PORT_INDEX_MAX = 0x17;
    constexpr uint32_t PIN_MAX = 7;

    constexpr uintptr_t pmr(uint32_t p) { return PMR_BASE + p; }

    // Console pins on PORTB (port index 0x0B): PB1/TXD6, PB0/RXD6.
    constexpr uintptr_t PORTB_PMR = PMR_BASE + 0x0B;
    constexpr uint8_t PB0 = 1u << 0; // RXD6
    constexpr uint8_t PB1 = 1u << 1; // TXD6

    // Diag LED = LED6 on P80, active-low (board Table 5-9). PORT8 = port index 8.
    constexpr uintptr_t PORT8_PDR = PDR_BASE + 8;
    constexpr uintptr_t PORT8_PODR = PODR_BASE + 8;
    constexpr uintptr_t PORT8_PMR = PMR_BASE + 8;
    constexpr uint8_t LED6 = 1u << 0; // P80
}

#endif
