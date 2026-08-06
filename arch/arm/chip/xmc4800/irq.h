// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 NVIC interrupt-node numbers (RM interrupt-node-assignment table).
// Clean-room from the XMC4700/XMC4800 Reference Manual (V1.3, 2016-07). The
// total line count (IRQ0..IRQ111) is KICKOS_MAX_IRQ in chip_limits.h.

#ifndef KICKOS_ARCH_ARM_CHIP_XMC4800_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_XMC4800_IRQ_H

namespace kickos::xmc::irq
{
    enum node : int
    {
        // USIC0 service-request 0. A wrong line silently never drains the console
        // TB ring (it fills and falls back to the bounded sync path).
        USIC0_SR0 = 84, // USIC0_0_IRQn
    };
}

#endif
