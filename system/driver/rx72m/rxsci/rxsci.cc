// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See <rxsci.h> for the interrupt sources and the ordering rules.

#include "rxsci.h"

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/declared/rxsci.h>
#include <kickos/sys/uart_console_desc.h>
#include <kickos/sys/uart_service.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace uart = kickos::uart;
namespace mmap = kickos::rx::mmap;

namespace
{
    constexpr uart::UartParams k_uart = {
        .announce = "[rxsci] device up (IRQ TX/RX)\n",
        // NO prime: TXI's only raise is a transfer taken with the source already armed, so a
        // pass that stopped with the ring loaded would wait on a transition that has already
        // happened. The announce leaves nothing loaded, and the call that takes a byte
        // disarms TIE.
        .prime = false
    };
}

// The baud 0 below keeps the divisor the kernel console left.
KICKOS_UART_CONSOLE_SERVICE(rxsci, k_uart, /*fallback_baud=*/0u);
