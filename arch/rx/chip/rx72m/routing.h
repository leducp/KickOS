// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_RX_CHIP_RX72M_ROUTING_H
#define KICKOS_ARCH_RX_CHIP_RX72M_ROUTING_H

#include <kickos/arch/rx_group.h> // GROUP_LINE_BASE: the core/chip line-space split

namespace kickos::rx::irq
{
    // TEI6 and ERI6 have no vector of their own: Table 15.7 (p.530) assigns TEI6 to GRPBL0.IS12
    // and ERI6 to GRPBL0.IS13, both reaching the CPU through the GROUPBL0 vector 110. line =
    // GROUP_LINE_BASE + group_index * GROUP_LINE_STRIDE + bit, group_index 0 = GROUPBL0 (the
    // group table in chip_rx72m.cc).
    //
    // Both are LEVEL sources, so a claimer must bind them KOS_IRQ_LEVEL and must clear the SCI6
    // flag itself (TEND via a TDR write or SCR.TEIE; ORER/FER/PER via the read-1-then-write-0
    // sequence) before the rearm, or the source re-asserts at once.
    enum group_line
    {
        SCI6_TEI = kickos::rxv3::GROUP_LINE_BASE + 12, // 268: GRPBL0.IS12 / GENBL0.EN12
        SCI6_ERI = kickos::rxv3::GROUP_LINE_BASE + 13, // 269: GRPBL0.IS13 / GENBL0.EN13
    };
}

#endif
