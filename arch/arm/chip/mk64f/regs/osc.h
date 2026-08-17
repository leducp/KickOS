// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 OSC0 register map (K64 Sub-Family RM ch.26). 8-bit registers.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_OSC_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_OSC_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::mk64f::reg::osc
{
    constexpr uintptr_t CR = mmap::OSC0_BASE + 0x00u; // 8-bit

    constexpr uint8_t CR_ERCLKEN = 1u << 7;
}

#endif
