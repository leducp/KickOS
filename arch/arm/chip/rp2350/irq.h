// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 NVIC interrupt/vector numbers (RP2350 datasheet
// RP-008373-DS-2, 3.2 Table 95). Line = IPSR - 16. 52 NVIC inputs (IRQ0..51;
// 46..51 spare). The console drains on UART1.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_IRQ_H

namespace kickos::rp2350::irq
{
    enum irq_num
    {
        UART1_IRQ = 34,   // PL011 UART1; only TXIM armed (drain ISR sole source)
        USBCTRL_IRQ = 14, // USB device controller; one line, level (INTS ORs the sources)
    };
}

#endif
