// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F UART0 class-driver leaf (driver-model rule 6): freestanding and stateless, a free function
// taking the module base. Built into kickos_class_mk64f on a bare include path (repo include/ and
// this chip's register dir only), so the same object links into the kernel or an unprivileged
// userspace driver. The register map (S1, RM 52.3.5) is the chip's regs/uart.h.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_UART_CLASS_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_UART_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace mk64f
{
namespace driver
{
    // True when UART0 can accept another byte into the data register (D). The K64F
    // UART register block is BYTE-mapped, so the read must be an 8-bit load: a 32-bit
    // load at base+4 spans S1/S2/C3/D and reading D pops the RX FIFO.
    bool uart0_tx_ready(uintptr_t base);
}
}
}

#endif
