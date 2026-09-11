// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (Xtensa LX6) UART0 buffered userspace UART driver.
//
// The console line cannot be claimed while the kernel's own TX ring holds it, so the publish
// MUST precede the claim: that ordering is KOS_DRV_EP_HANDOVER's.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include "irq.h"
#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::esp32::mmap;
namespace lx6irq = kickos::esp32::irq;

namespace
{
    constexpr uart::UartParams k_uart = {
        .open_fail = "[lx6uart] UART0 open refused: divisor read-back or frame",
        .announce = "[lx6uart] device up (IRQ TX/RX)\n",
        .prime = true
    };
}

// No kos_periph_enable and no DPORT clock ungate: the ROM ran its boot log through UART0, so
// the block is already clocked out of reset.
//
// The baud 0 below keeps the divisor the ROM left, and the frame is Register 19.9's reset
// framing, so the backend has nothing to reprogram.
//
// LEVEL: CPU interrupt 13 is level-triggered (TRM Table 8.3-2) and the UART latch stays set
// until the driver clears it.
KICKOS_UART_CONSOLE_SERVICE(lx6uart, k_uart, /*fallback_baud=*/0u, mmap::UART0_BASE,
                            lx6irq::CONSOLE_TX_LINE, KOS_IRQ_LEVEL, "lx6uartirq");
