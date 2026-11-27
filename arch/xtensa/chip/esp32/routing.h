// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_ROUTING_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_ROUTING_H

namespace kickos::esp32::irq
{
    // TRM Table 8.3-1 (p.176): the matrix source of each UART.
    enum periph_src
    {
        UART0_SRC = 34,
        UART1_SRC = 35,
        UART2_SRC = 36,
    };

    // TRM Table 8.3-2 fixes each number's type and priority. Every one here must be
    // Level-Triggered at priority 1, so the level-1 vector takes it (KICKOS_L1_INT_MASK): a level
    // input's pending state follows the trigger, so clearing the trigger is the whole
    // acknowledgement. 0, 1, 2, 3, 4, 5, 8, 9, 12, 13, 17 and 18 qualify. Each UART has its own,
    // so masking one UART's line never masks another's.
    enum cpu_int
    {
        UART0_CPU_INT = 13,
        UART1_CPU_INT = 17,
        UART2_CPU_INT = 18,
        DOORBELL_CPU_INT = 12,
    };

    // The core that takes a device line until a claim routes it.
    enum dev_core
    {
        DEVICE_CORE = 0,
    };
}

#endif
