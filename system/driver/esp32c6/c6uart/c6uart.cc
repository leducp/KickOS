// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 UART0 buffered userspace UART driver.
//
// The grouped UART0 line cannot be claimed while the kernel's own TX ring holds it, so the
// publish MUST precede the claim: that ordering is KOS_DRV_EP_HANDOVER's.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::esp32c6::mmap;
namespace c6irq = kickos::esp32c6::irq;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[c6uart] UART0 open refused: source clock, divisor or frame",
        .announce = "[c6uart] device up (IRQ TX/RX)\n",
        .prime = true
    };
}

// No kos_periph_enable: the PMP grant carries the window, PCR leaves UART0's bus clock
// ungated out of reset, and arch_init's HP_APM REE0 permit already covers the block.
//
// The baud 0 below keeps the divisor the ROM left.
//
// LEVEL: the UART source stays asserted until the driver clears the latch.
KICKOS_UART_CONSOLE_SERVICE(c6uart, k_uart, /*fallback_baud=*/0u, mmap::UART0_BASE,
                            c6irq::UART0_TX_LINE, KOS_IRQ_LEVEL, "c6uartirq");
