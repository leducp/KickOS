// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F DSPI class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: free functions taking the DSPI module base
// explicitly, no ctor/dtor, no mutable static state. Built as its own
// kickos_class_mk64f static lib on a bare include path (repo include/ + this
// chip's register dir ONLY, never kernel/include), so the SAME object links
// unchanged into BOTH the kernel and an unprivileged userspace DSPI driver. One
// shared read-only copy in the MPU-partitioned ELF, so no writable state.
//
// The register map it reads (SR, RM 50.3.5) comes from the chip's shared
// regs/dspi.h, not a local copy.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_DSPI_CLASS_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_CLASS_DSPI_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace mk64f
{
namespace driver
{
    // Number of received words waiting in the 4-deep RX FIFO. Pure read of SR, so
    // side-effect-free: the W1C flags in SR clear only on a write.
    uint32_t dspi_rx_count(uintptr_t base);
}
}
}

#endif
