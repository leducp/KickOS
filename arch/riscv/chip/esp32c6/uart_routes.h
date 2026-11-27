// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Each ESP32-C6 UART's logical line and its INTMTX INT_STATUS_1 bit.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_UART_ROUTES_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_UART_ROUTES_H

#include "esp_uart_routes.h"
#include "irq.h"
#include "regs/intmtx.h"

namespace kickos::esp32c6
{
    constexpr kickos::esp::UartRoute UART_ROUTES[] = {
        {irq::UART0_IRQ, reg::intmtx::UART0_STATUS_1},
        {irq::UART1_IRQ, reg::intmtx::UART1_STATUS_1},
    };
}

#endif
