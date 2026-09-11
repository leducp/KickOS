// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 buffered IRQ-driven userspace UART driver (see k64uartirq.h).
//
// HARD RULE (design D7): NO libc stdio anywhere in this file. printf/puts route through
// _write -> kos_send(cap 0) -> this driver's own endpoint, and the service thread holds the
// only WAIT cap on it, so a self-send never returns.

#include "k64uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include <irq.h> // UART0_RXTX_IRQ
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::mk64f::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[k64uartirq] UART0 open refused: AIPS enable, baud request or frame",
        .announce = "[k64uartirq] device up (IRQ TX/RX)\n",
        .prime = true
    };
}

// A cfg naming no rate asks for 115200, not for the kernel's divisor: this driver reprograms
// the frame anyway, so the divisor is written in the same pass.
//
// UART0_RXTX is claimed BY NUMBER, so a cfg naming another instance would grant one window
// and interrupt on another. Leg L9 refuses the descriptor without the base below.
//
// LEVEL: every source on this vector is a status flag that stays asserted until the driver
// clears it at the peripheral.
KICKOS_UART_CONSOLE_SERVICE(k64uartirq, k_uart, /*fallback_baud=*/115200u, mmap::UART0_BASE,
                            kickos::mk64f::irq::UART0_RXTX_IRQ, KOS_IRQ_LEVEL, "uartirq");
