// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_GIC_PPI_H
#define KICKOS_ARCH_GIC_PPI_H

namespace kickos::arm64
{
    // The Non-secure EL1 physical timer; the number is architecturally assigned by the GIC.
    constexpr int PPI_EL1_PHYS_TIMER = 30;
}

#endif
