// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 IRQ-driven buffered UART console driver on USIC0 CH0 (see
// <kickos/driver/xmcuartirq.h>).
//
// The USIC behaviour cited below is clean-room from the XMC4700/XMC4800 Reference Manual
// (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. "RM p.NN" are the manual's printed
// pages.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuartirq.h>

#include <kickos/driver/declared/xmcuartirq.h>
#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::xmc::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .announce = "[xmcuartirq] device up (IRQ TX)\n",
        // EDGE and still primed: kos_uart_open has already set TBIEN, so the announce's
        // polled writes raise transmit-buffer events that the first irq_wait discards.
        .prime = true
    };
}

// The baud 0 below keeps the divisor the kernel left, and the frame is what
// kickos_xmc_usic_init programmed, so the backend has nothing to reprogram.
KICKOS_UART_CONSOLE_SERVICE(xmcuartirq, k_uart, /*fallback_baud=*/0u);
