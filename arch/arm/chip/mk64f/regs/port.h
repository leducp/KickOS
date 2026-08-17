// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 PORT pin-control register map (K64 Sub-Family RM). PCRn = PORTx base +
// pin*4. Offsets/fields are instance-relative to a PORTx base (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_PORT_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_PORT_H

#include <stdint.h>

namespace kickos::mk64f::reg::port
{
    constexpr uintptr_t PCR_STRIDE = 4u; // PCRn = base + pin*PCR_STRIDE

    // PCR MUX field [10:8]: ALT selection.
    constexpr uint32_t PCR_MUX_SHIFT = 8u;
    constexpr uint32_t PCR_MUX_MASK = 7u;
    constexpr uint32_t PCR_MUX_GPIO = 1u << 8; // MUX=001 = GPIO (ALT1)
    constexpr uint32_t PCR_MUX_ALT3 = 3u << 8; // MUX=011 = ALT3
}

#endif
