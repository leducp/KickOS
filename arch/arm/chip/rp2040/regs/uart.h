// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2040 console UART, UART0.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_UART_H

#include "../../rp2xxx/pl011_baud.h"
#include "clocks.h"
#include "xosc.h"

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::uart
{
    constexpr uintptr_t BASE = mmap::UART0_BASE;
#include "../../rp2xxx/regs/uart.inc"
}

#endif
