// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M IRQ / interrupt-vector NUMBERS the chip backend programs directly. From the
// RX72M Group User's Manual: Hardware (r01uh0804ej0120, Rev.1.20) sec.15.3.1 interrupt
// vector table; hand-rolled, clean-room.

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_IRQ_H
#define KICKOS_ARCH_RX_CHIP_RX72M_IRQ_H

#include <kickos/arch/rx_group.h> // GROUP_LINE_BASE: the core/chip line-space split

namespace kickos::rx::irq
{
    enum vector
    {
        // SCI6 receive-data-full (RXI6) and transmit-data-empty (TXI6): the two
        // DEDICATED SCI6 vectors, IER0A.IEN6 / IER0A.IEN7, IPR086 / IPR087
        // (UM sec.15.3.1 Table 15.5 p.523). Both edge-detected; for both the IPR
        // index equals the vector number. INTB[86] and INTB[87] have their own
        // first-level ISRs (startup.S).
        SCI6_RXI = 86,
        SCI6_TXI = 87,
    };

    // Group-source LOGICAL LINES. TEI6 and ERI6 have no vector of their own: Table 15.5
    // lists only RXI6 and TXI6 for SCI6, and Table 15.7 (p.530) assigns TEI6 to
    // GRPBL0.IS12 and ERI6 to GRPBL0.IS13, both reaching the CPU through the GROUPBL0
    // vector 110. So they are addressed as logical lines above the vector space:
    // line = GROUP_LINE_BASE + group_index * GROUP_LINE_STRIDE + bit, group_index 0 =
    // GROUPBL0 (see the group table in chip_rx72m.cc).
    //
    // Both are LEVEL sources, so a claimer must bind them KOS_IRQ_LEVEL and must clear
    // the SCI6 flag itself (TEND via a TDR write or SCR.TEIE; ORER/FER/PER via the
    // read-1-then-write-0 sequence) before the rearm, or the source re-asserts at once.
    enum group_line
    {
        SCI6_TEI = kickos::rxv3::GROUP_LINE_BASE + 12, // 268: GRPBL0.IS12 / GENBL0.EN12
        SCI6_ERI = kickos::rxv3::GROUP_LINE_BASE + 13, // 269: GRPBL0.IS13 / GENBL0.EN13
    };
}

#endif
