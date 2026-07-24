// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 (FRDM-K64F) NVIC interrupt/vector numbers (K64 Sub-Family Reference
// Manual, interrupt-vector-assignments table). Line = IPSR - 16.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_IRQ_H

namespace kickos::mk64f::irq
{
    enum irq_num
    {
        UART0_RXTX_IRQ = 31, // UART0 status sources (RX/TX combined)
        UART0_ERR_IRQ = 32,  // UART0 error
    };
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_IRQ_H
