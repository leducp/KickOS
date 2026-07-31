// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M IRQ / interrupt-vector NUMBERS the chip backend programs directly
// (Phase-1 register consolidation). From the RX72M Group User's Manual: Hardware
// (r01uh0804ej0120, Rev.1.20) sec.15.3.1 interrupt vector table; hand-rolled,
// clean-room. ADDITIVE: duplicates the literals still inline in chip_rx72m.cc.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_IRQ_H
#define KICKOS_ARCH_RX_CHIP_RX72M_IRQ_H

namespace kickos::rx::irq
{
    enum vector
    {
        // SCI6 transmit-data-empty (TXI6): dedicated vector 87, IER0A.IEN7,
        // IPR087 (UM sec.15.3.1). Edge-triggered; for this line the IPR index
        // equals the vector number. INTB[87] -> the console-drain ISR (startup.S).
        SCI6_TXI = 87,

        // SCI6 transmit-end (TEI6) / receive-error (ERI6): GROUPBL0 sources, NOT
        // yet implemented (see the header note). Placeholder value -1 until the
        // grouped-interrupt path lands.
        SCI6_TEI = -1,
        SCI6_ERI = -1,
    };
}

#endif
