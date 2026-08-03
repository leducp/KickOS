// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M MPC (multi-function pin controller) register offsets + fields for the SCI6
// console pins. From the RX72M Group User's Manual: Hardware (r01uh0804ej0120,
// Rev.1.20) sec.23; hand-rolled, clean-room. Bases: mmap.h.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_MPC_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_MPC_H

#include <stdint.h>

#include "../mmap.h"

namespace kickos::rx::reg::mpc
{
    constexpr uintptr_t PWPR = mmap::MPC + 0x1F;  // write-protect (sec.23.2.1)
    constexpr uint8_t PWPR_PFSWE = 1u << 6;       // PFS register write enable
    constexpr uint8_t PWPR_B0WI = 1u << 7;        // PFSWE write disable

    // Per-pin PFS block: PnmPFS = PFS + port*8 + bit (UM sec.23.2). PORTB = 0x0B.
    // The stride-8 formula holds across the whole file, PORT0 @0x0008C140 through
    // PORTQ @0x0008C1F8 (sec.23.2.2-23.2.24 address lists).
    constexpr uintptr_t PFS = mmap::MPC + 0x40;   // PFS block base (0x0008C140)
    constexpr uintptr_t PB0PFS = PFS + 0x0B * 8 + 0; // PB0 pin function
    constexpr uintptr_t PB1PFS = PFS + 0x0B * 8 + 1; // PB1 pin function
    constexpr uint8_t PFS_PSEL_SCI6 = 0x0B;       // PSEL=001011b: PB0->RXD6, PB1->TXD6

    constexpr uintptr_t pfs(uint32_t port, uint32_t pin) { return PFS + port * 8u + pin; }

    // arch_pinmux_set `func` encoding for this chip: [7:0] the PmnPFS byte written
    // verbatim (PSEL[5:0] | ISEL bit6 | ASEL bit7, sec.23.2.2; every bit is defined, so
    // no sub-byte reserved mask), [8] the PORTm.PMR bit value, [9] arm the PFS write.
    // [31:10] must be zero, so a stale word cannot be silently accepted once ODR/DSCR/PCR
    // join the encoding.
    constexpr uint32_t PINMUX_PFS_MASK = 0x000000FFu;
    constexpr uint32_t PINMUX_PMR = 1u << 8;     // 1 = peripheral function, 0 = general I/O
    constexpr uint32_t PINMUX_PFS_EN = 1u << 9;  // arm the PmnPFS write
    constexpr uint32_t PINMUX_RESERVED = 0xFFFFFC00u;
}

#endif
