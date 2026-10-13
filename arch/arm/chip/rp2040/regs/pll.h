// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PLL_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg
{
#include "../../rp2xxx/regs/pll.inc"
}

namespace kickos::rp2040::reg::pll
{
    constexpr uint32_t PRIM_POSTDIV = (6u << 16) | (2u << 12); // POSTDIV1=6, POSTDIV2=2: 125 MHz
}

#endif
