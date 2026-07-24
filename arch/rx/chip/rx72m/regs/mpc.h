// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M MPC (multi-function pin controller) register offsets + fields for the
// SCI6 console pins (Phase-1 register consolidation). From the RX72M Group
// User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.23; hand-rolled,
// clean-room. ADDITIVE: duplicates the literals still inline in chip_rx72m.cc.
// Bases: mmap.h.

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
    constexpr uintptr_t PFS = mmap::MPC + 0x40;   // PFS block base (0x0008C140)
    constexpr uintptr_t PB0PFS = PFS + 0x0B * 8 + 0; // PB0 pin function
    constexpr uintptr_t PB1PFS = PFS + 0x0B * 8 + 1; // PB1 pin function
    constexpr uint8_t PFS_PSEL_SCI6 = 0x0B;       // PSEL=001011b: PB0->RXD6, PB1->TXD6
}

#endif
