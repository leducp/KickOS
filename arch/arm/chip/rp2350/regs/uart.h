// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 PL011 UARTs.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_UART_H

#include "../../rp2xxx/pl011_baud.h"
#include "clocks.h"
#include "xosc.h"

#include <stdint.h>

namespace kickos::rp2350::reg::uart
{
#include "../../rp2xxx/regs/uart.inc"
}

#endif
