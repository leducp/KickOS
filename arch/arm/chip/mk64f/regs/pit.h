// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 PIT register map (K64 Sub-Family RM ch.44). The kernel time base
// chains ch0/ch1 into a free-running 64-bit down-counter.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_PIT_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_PIT_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::mk64f::reg::pit
{
    constexpr uintptr_t MCR = mmap::PIT_BASE + 0x000u;
    constexpr uintptr_t LDVAL0 = mmap::PIT_BASE + 0x100u;
    constexpr uintptr_t CVAL0 = mmap::PIT_BASE + 0x104u;
    constexpr uintptr_t TCTRL0 = mmap::PIT_BASE + 0x108u;
    constexpr uintptr_t LDVAL1 = mmap::PIT_BASE + 0x110u;
    constexpr uintptr_t CVAL1 = mmap::PIT_BASE + 0x114u;
    constexpr uintptr_t TCTRL1 = mmap::PIT_BASE + 0x118u;

    constexpr uint32_t TCTRL_TEN = 1u << 0; // timer enable
    constexpr uint32_t TCTRL_CHN = 1u << 2; // chain to the previous channel
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_PIT_H
