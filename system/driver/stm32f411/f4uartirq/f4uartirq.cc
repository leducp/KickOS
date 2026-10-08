// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// STM32F411/USART2 buffered IRQ-driven userspace UART driver (see f4uartirq.h).
//
// This task's stdout is the kernel console, which the wire does not carry while this driver
// owns the UART.

#include "f4uartirq.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/declared/f4uartirq.h>
#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::stm32f411::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .announce = "[f4uartirq] device up (IRQ TX/RX)\n",
        // prime=false is safe here: a LEVEL rearm discards the latch and then unmasks into a
        // line the peripheral still drives, so an RXNE that arrived during the announce
        // raises again at the first irq_wait rather than being lost. The announce's own
        // polled writes leave nothing armed on the TX side: kos_uart_write disarms TXEIE on
        // every call the device accepted whole.
        .prime = false
    };
}

// 115200, not the kernel's divisor: this driver rewrites CR1/CR2/CR3 anyway, so BRR is written
// in the same pass.
KICKOS_UART_CONSOLE_SERVICE(f4uartirq, k_uart, /*fallback_baud=*/115200u);
