// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411/USART2 buffered IRQ-driven userspace UART driver (see f4uartirq.h).
//
// HARD RULE (design D7): NO libc stdio anywhere in this file. printf/puts route through
// _write -> kos_send(cap 0) -> this driver's own endpoint, and the service thread holds the
// only WAIT cap on it, so a self-send never returns.

#include "f4uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include <irq.h> // USART2_IRQ
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::stm32f411::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[f4uartirq] USART2 open refused: baud request or frame",
        .announce = "[f4uartirq] device up (IRQ TX/RX)\n",
        // prime=false is safe here: a LEVEL rearm discards the latch and then unmasks into a
        // line the peripheral still drives, so an RXNE that arrived during the announce
        // raises again at the first irq_wait rather than being lost. The announce's own
        // polled writes leave nothing armed on the TX side: kos_uart_write disarms TXEIE on
        // every call the device accepted whole.
        .prime = false
    };
}

// A cfg naming no rate asks for 115200, not for the kernel's divisor: this driver rewrites
// CR1/CR2/CR3 anyway, so BRR is written in the same pass.
//
// The USART2 vector is claimed BY NUMBER, so a cfg naming USART1 or USART6 would grant one
// window and interrupt on another. Leg L9 refuses the descriptor without the base below.
//
// LEVEL: every USART event is ORed into one request line (RM0383 sec.19.4 Figure 191), so a
// status flag stays asserted until the driver clears it at the peripheral. ORE is the one
// that matters: RXNEIE arms it, and a bare DR read does not clear it.
KICKOS_UART_CONSOLE_SERVICE(f4uartirq, k_uart, /*fallback_baud=*/115200u, mmap::USART2_BASE,
                            kickos::stm32f411::irq::USART2_IRQ, KOS_IRQ_LEVEL, "uartirq");
