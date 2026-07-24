// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M ICU register instances the chip backend programs directly (Phase-1
// register consolidation). From the RX72M Group User's Manual: Hardware
// (r01uh0804ej0120, Rev.1.20) sec.15; hand-rolled, clean-room. ADDITIVE:
// duplicates the literals still inline in chip_rx72m.cc. Bases: mmap.h.
//
// NOTE: the generic ICU register FILE (IR/IER/IPR bases, SWINTR, the vector->IPR
// mapping) is defined at the arch layer in arch/rx/rxv3/regs.h (kickos::rxv3).
// This header holds only the two specific register instances the CHIP touches to
// bring up the CMTW0 timer line.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_REGS_ICU_H
#define KICKOS_ARCH_RX_CHIP_RX72M_REGS_ICU_H

#include <stdint.h>

#include "../mmap.h"

namespace kickos::rx::reg::icu
{
    // IER byte holding vector 30 (CMWI0): ICU.IER03, IEN6 (UM sec.15.2.2).
    constexpr uintptr_t IER03 = mmap::ICU + 0x203;
    constexpr uint8_t IER03_CMWI0 = 1u << 6; // IER03.IEN6 = vector 30 (CMWI0)
    // IPR for CMWI0 (UM interrupt table).
    constexpr uintptr_t IPR006 = mmap::ICU + 0x306;
}

#endif
