// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) NVIC interrupt/vector numbers (i.MX RT1060
// Processor Reference Manual, Rev. 3, IMXRT1060RM, Table 4-2). Line = IPSR - 16.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_IRQ_H

namespace kickos::imxrt1062::irq
{
    enum irq_num
    {
        LPUART6_IRQ = 25, // LPUART6 combined TX/RX (RM Table 4-2)
    };
}

#endif // KICKOS_ARCH_ARM_CHIP_IMXRT1062_IRQ_H
