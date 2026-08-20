// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RXv3 core/chip contract for GROUP-interrupt logical lines. Both sides need it: the
// core branches arch_irq_mask / arch_irq_unmask / arch_irq_clear_pending on the split, the
// chip owns the group -> register mapping behind kickos_rx_group_arm.
//
// An RX group vector collapses up to 32 sources onto ONE vector (RX72M UM sec.15.4.4),
// so a group source has no vector of its own, no ICU.IR flag of its own and no ICU.IER
// bit of its own: its kernel-owned mask is GENxxx.ENj. It cannot be numbered inside the
// 256-entry vector space, so it is numbered above it:
//
//     line = GROUP_LINE_BASE + group_index * GROUP_LINE_STRIDE + bit
//
// The range must not overlap the low software-inject space (< SOFT_IRQ_LINES), or
// arch_irq_inject would alias a real level source.
//
// A board that raises KICKOS_MAX_IRQ to cover this range pays 8 bytes of kernel .bss per
// line (Kernel::irq_table), so it covers only the groups the chip actually has.

#ifndef KICKOS_ARCH_RX_GROUP_H
#define KICKOS_ARCH_RX_GROUP_H

namespace kickos
{
    namespace rxv3
    {
        constexpr int GROUP_LINE_BASE = 256;
        constexpr int GROUP_LINE_STRIDE = 32;
    }
}

// Arm a REAL ICU vector's IPR/IER, without the group dispatch that arch_irq_mask and
// arch_irq_unmask do first. kickos_rx_group_arm arms the group's own vector and must come
// through here: routing it back through the seam is a cycle to any call-graph analysis,
// terminating only on the value-range invariant that a group vector is below
// GROUP_LINE_BASE, which no graph can see. Caller holds arch_irq_save; `line` must be a
// real vector, neither a soft line nor a group line.
extern "C" void kickos_rx_icu_line_arm(int line, int on);

#endif
