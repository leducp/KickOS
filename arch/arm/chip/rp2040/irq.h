// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2040 (Pico) NVIC interrupt/vector numbers (RP2040 datasheet,
// RP-008371-DS, interrupt table). The RP2040 wires 26 lines (IRQ0..25); line =
// IPSR - 16. Only TXIM is armed on UART0, so its drain ISR is the sole source.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_IRQ_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_IRQ_H

namespace kickos::rp2040::irq
{
    enum irq_num
    {
        USBCTRL_IRQ = 5, // USB device controller; one line, level (INTS ORs the sources)
        UART0_IRQ = 20,  // ARM PL011 UART0
        UART1_IRQ = 21,  // ARM PL011 UART1
    };
}

#endif
