// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 GPIO controller register map (K64 Sub-Family RM ch.55). Offsets are
// instance-relative to a GPIOx base (see mmap.h); direction is a separate PDDR
// write (unlike XMC, where it lives in the mux).

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_GPIO_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_GPIO_H

#include <stdint.h>

namespace kickos::mk64f::reg::gpio
{
    constexpr uintptr_t PSOR_OFFSET = 0x04u; // set -> high
    constexpr uintptr_t PCOR_OFFSET = 0x08u; // clear -> low
    constexpr uintptr_t PDIR_OFFSET = 0x10u; // input data
    constexpr uintptr_t PDDR_OFFSET = 0x14u; // 1 = output
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_GPIO_H
