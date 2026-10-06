// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_ROUTING_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_ROUTING_H

namespace kickos::esp32::irq
{
    // TRM Table 8.3-1: source 34 is UART_INTR, mapped by DPORT_PRO_UART_INTR_MAP_REG (TRM 12.4).
    enum periph_src
    {
        UART0_SRC = 34,
    };

    // TRM Table 8.3-2 fixes each number's type and priority. 13 is Peripheral, Level-Triggered,
    // priority 1, so the level-1 vector takes it (bit 13 is in KICKOS_L1_INT_MASK).
    enum cpu_int
    {
        UART0_CPU_INT = 13,
        // Must be Level-Triggered at priority 1: a level input's pending state follows the
        // trigger register, so clearing the trigger is the whole acknowledgement. 0, 1, 2, 3, 4,
        // 5, 8, 9, 12, 17 and 18 all qualify.
        DOORBELL_CPU_INT = 12,
    };

    // The core that takes a device line until a claim routes it.
    enum dev_core
    {
        CONSOLE_CORE = 0,
    };
}

#endif
