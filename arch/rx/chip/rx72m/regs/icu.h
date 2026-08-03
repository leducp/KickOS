// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M ICU register instances the chip backend programs directly. From the RX72M
// Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.15; hand-rolled,
// clean-room. Bases: mmap.h.
//
// The generic ICU register FILE (IR/IER/IPR bases, SWINTR, the vector -> IPR mapping)
// is defined at the arch layer in arch/rx/rxv3/regs.h (kickos::rxv3); this header
// holds only what the CHIP touches.

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

    // --- Group interrupts (UM sec.15.2.23 p.501, sec.15.2.24 p.503, Table 15.7 p.530) ---
    // Five groups on this part, all LEVEL-detected, each collapsing up to 32 sources onto
    // one vector. Three facts drive the demux:
    //
    //  1. GRPxxx is READ-ONLY, and GRPxxx.ISj "becomes 1 only when the corresponding ENj
    //     bit in the group interrupt request enable register is 1" (sec.15.2.23 p.502).
    //     The AND is in hardware (Fig.15.3 p.529), so a demux must NOT re-AND with GENxxx,
    //     and must not treat a read of 0 as "masked but pending".
    //  2. Clearing ENj clears an already-set ISj AND the group's IRn (sec.15.2.24(3)
    //     p.504, sec.15.5.4 p.542; IRn.IR itself is unwritable for a level source,
    //     sec.15.2.1 p.479 note 1). Masking a level group source at GENxxx is therefore a
    //     true controller mask, which is what lets it serve as the tier-1 kernel mask.
    //     IRn is the OR over the group (Fig.15.17 p.542), so it only falls when every
    //     other enabled source in the same group is also quiet: masking one source is
    //     still a real mask of THAT source, but the vector may stay asserted for a sibling.
    //     The EDGE groups IE0/BE0 are the opposite ("even when the ENj bit is set to 0,
    //     the ISj flag does not change") and are not used here.
    //  3. These groups have NO clear register: sec.15.2.25 p.505 defines GCRIE0/GCRBE0
    //     for the edge groups only, and Table 15.7's clear-bit column is empty for every
    //     BL/AL row. So the only other way down is the peripheral's own flag write, which
    //     belongs to the driver that owns the peripheral (RULE L1).
    constexpr uintptr_t GRPBL0 = mmap::ICU + 0x630; // 0008 7630h
    constexpr uintptr_t GRPBL1 = mmap::ICU + 0x634;
    constexpr uintptr_t GRPBL2 = mmap::ICU + 0x638;
    constexpr uintptr_t GRPAL0 = mmap::ICU + 0x830; // 0008 7830h
    constexpr uintptr_t GRPAL1 = mmap::ICU + 0x834;
    constexpr uintptr_t GENBL0 = mmap::ICU + 0x670; // 0008 7670h
    constexpr uintptr_t GENBL1 = mmap::ICU + 0x674;
    constexpr uintptr_t GENBL2 = mmap::ICU + 0x678;
    constexpr uintptr_t GENAL0 = mmap::ICU + 0x870; // 0008 7870h
    constexpr uintptr_t GENAL1 = mmap::ICU + 0x874;

    // Group vectors (UM sec.15.3.1 Table 15.5 p.523). Level-detected, and none of them
    // can start the DTC or DMAC (the table's columns read N/A). IPR index == vector.
    constexpr int GROUPBL0_VECTOR = 110; // IER0D.IEN6, IPR110
    constexpr int GROUPBL1_VECTOR = 111; // IER0D.IEN7, IPR111
    constexpr int GROUPBL2_VECTOR = 107; // IER0D.IEN3, IPR107
    constexpr int GROUPAL0_VECTOR = 112; // IER0E.IEN0, IPR112
    constexpr int GROUPAL1_VECTOR = 113; // IER0E.IEN1, IPR113

    // The reserved-block span has to cover the group registers, not just IR/IER/IPR. The
    // last word the kernel touches is GENAL1 + 4 == 0x878, rounded up here.
    constexpr uintptr_t SPAN = 0x880;
}

#endif
