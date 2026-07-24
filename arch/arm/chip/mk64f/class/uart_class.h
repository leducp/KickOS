// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F UART0 class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: a free function taking the UART module base
// explicitly, no ctor/dtor, no mutable static state. Built as part of the
// kickos_class_mk64f static lib on a bare include path (repo include/ + this
// chip's register dir ONLY, never kernel/include), so the SAME object could link
// unchanged into BOTH the kernel and an unprivileged userspace UART driver.
//
// The kernel console (chip_mk64f.cc arch_console_write_sync / k64_tx_slot_free)
// reads the same S1.TDRE bit and could consume this leaf later (the Rule 3
// proof-obligation). That refactor is DEFERRED to Phase A: routing the kernel's
// panic-critical console through a kickos_class link edge is out of scope here.
// Today the leaf serves only the userspace k64uart driver.
//
// The register map it reads (S1, RM 52.3.5) comes from the chip's shared
// regs/uart.h -- the leaf sources the offset/field from there, not a local copy.

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

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_UART_CLASS_H
