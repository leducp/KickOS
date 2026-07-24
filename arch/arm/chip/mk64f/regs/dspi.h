// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 DSPI register map (K64 Sub-Family RM ch.50). Offsets are instance-
// relative to a DSPIn base.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_DSPI_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_DSPI_H

#include <stdint.h>

namespace kickos::mk64f::reg::dspi
{
    // Status Register (RM 50.3.5): SR.RXCTR[7:4] = RX FIFO fill level.
    constexpr uintptr_t SR_OFFSET = 0x2Cu;
    constexpr uint32_t SR_RXCTR_SHIFT = 4u;
    constexpr uint32_t SR_RXCTR_MASK = 0xFu;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_DSPI_H
