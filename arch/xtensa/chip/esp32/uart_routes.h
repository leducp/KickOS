// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Each ESP32 UART's logical line and the CPU interrupt it is routed to.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_UART_ROUTES_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_UART_ROUTES_H

#include "esp_uart_routes.h"
#include "irq.h"
#include "routing.h"

namespace kickos::esp32
{
    constexpr kickos::esp::UartRoute UART_ROUTES[] = {
        {irq::UART0_IRQ, 1u << irq::UART0_CPU_INT},
        {irq::UART1_IRQ, 1u << irq::UART1_CPU_INT},
        {irq::UART2_IRQ, 1u << irq::UART2_CPU_INT},
    };
}

#endif
