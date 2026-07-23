// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F DSPI class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: free functions taking the DSPI module base
// explicitly, no ctor/dtor, no mutable static state. Built as its own
// kickos_class_mk64f static lib on a bare include path (repo include/ + this
// chip's register dir ONLY -- never kernel/include), so the SAME object links
// unchanged into BOTH the kernel (Phase A timebase, deferred) and an unprivileged
// userspace DSPI driver. One shared read-only copy in the MPU-partitioned ELF, so
// no writable state.
//
// The K64F chip layer keeps its register map inline (no shared chip header), so
// this leaf owns the one DSPI register it reads -- SR (RM 50.3.5); it does not
// duplicate a definition that lives anywhere else.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_DSPI_CLASS_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_DSPI_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace mk64f
{
namespace classdrv
{
    // DSPI Status Register (RM 50.3.5): SR.RXCTR[7:4] = RX FIFO fill level.
    constexpr uintptr_t DSPI_SR_OFFSET = 0x2Cu;
    constexpr uint32_t DSPI_SR_RXCTR_SHIFT = 4u;
    constexpr uint32_t DSPI_SR_RXCTR_MASK = 0xFu;

    // Number of received words waiting in the 4-deep RX FIFO. Pure read of SR --
    // side-effect-free (the W1C flags in SR clear only on a write). Shared drain
    // predicate for the userspace polled-FIFO driver and any future kernel user.
    uint32_t dspi_rx_count(uintptr_t base);
}
}
}

#endif
