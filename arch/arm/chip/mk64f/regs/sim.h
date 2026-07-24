// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 SIM register map (K64 Sub-Family RM ch.12): clock gates + dividers.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SIM_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SIM_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::mk64f::reg::sim
{
    constexpr uintptr_t SCGC4 = mmap::SIM_BASE + 0x34u;
    constexpr uintptr_t SCGC5 = mmap::SIM_BASE + 0x38u;
    constexpr uintptr_t SCGC6 = mmap::SIM_BASE + 0x3Cu;
    constexpr uintptr_t CLKDIV1 = mmap::SIM_BASE + 0x44u;

    constexpr uint32_t SCGC4_UART0 = 1u << 10;
    constexpr uint32_t SCGC5_PORTB = 1u << 10;
    constexpr uint32_t SCGC6_PIT = 1u << 23;

    // SCGC5 per-PORT clock-gate bit: PORTx = bit (9 + port), A=0..E=4.
    constexpr uint32_t SCGC5_PORT_SHIFT = 9u;

    // CLKDIV1 divide fields are (divide-1) in nibbles [31:16]: OUTDIV1[31:28] core,
    // OUTDIV2[27:24] bus, OUTDIV3[23:20] FlexBus, OUTDIV4[19:16] flash.
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_REGS_SIM_H
