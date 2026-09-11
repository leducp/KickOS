// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The <kickos/driver/uart.h> bodies the ESP32 (Xtensa LX6) and the ESP32-C6 (RV32) UART0
// backends share: everything below kos_uart_open. The two parts are on DIFFERENT ISAs and
// link no archive in common, so uart_esp.cc enters each DRIVER's own archive, named by that
// driver's CMakeLists, and reads the configured chip's own <regs/uart.h>.
//
// A chip joins the family by aliasing its register namespace into kickos::espuart (its
// regs/uart.h) and by spelling what this unit names: reg::uart's OFF_FIFO, OFF_INT_ST,
// OFF_INT_ENA, OFF_INT_CLR, OFF_STATUS, TXFIFO_CNT_S / _MASK, RXFIFO_CNT_S / _MASK,
// TXFIFO_LIMIT, and the RXFIFO_FULL / TXFIFO_EMPTY / PARITY_ERR / FRM_ERR / RXFIFO_OVF
// interrupt bits.
//
// WHAT IS NOT SHARED, and must not become so: kos_uart_open. The LX6 keeps the framing and
// the divisor the ROM left and refuses any change; the C6 programs both, across a _SYNC
// commit protocol the LX6 has no counterpart for.

#ifndef KICKOS_SYSTEM_DRIVER_ESPUART_UART_ESP_H
#define KICKOS_SYSTEM_DRIVER_ESPUART_UART_ESP_H

#include <regs/uart.h>

#include <stdint.h>

namespace kickos::espuart
{
    // TX-empty is armed on demand by kos_uart_write; an open arms the RX sources alone.
    constexpr uint32_t RX_INT_MASK = reg::uart::RXFIFO_FULL_INT | reg::uart::RXFIFO_OVF_INT
                                     | reg::uart::FRM_ERR_INT | reg::uart::PARITY_ERR_INT;

    // Bounds every register poll in the family, far longer than any legitimate wait.
    constexpr uint32_t POLL_MAX = 1000000u;
}

#endif
