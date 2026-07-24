// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 PADS_BANK0 register map (RP2040 datasheet, RP-008371-DS, 2.19.6): per-pin
// pad control (drive, pulls, input-enable, output-disable). GPIOn pad = base +
// 0x04 + n*4 (VOLTAGE_SELECT sits at offset 0x00).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PADS_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PADS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::pads
{
    constexpr uintptr_t GPIO_BASE_OFFSET = 0x04u; // GPIO0 pad register
    constexpr uintptr_t STRIDE = 0x4u;

    constexpr uintptr_t gpio(uint32_t n) { return mmap::PADS_BANK0_BASE + GPIO_BASE_OFFSET + n * STRIDE; }

    constexpr uintptr_t GPIO0 = gpio(0);
    constexpr uintptr_t GPIO1 = gpio(1);
    constexpr uintptr_t GPIO25 = gpio(25);

    constexpr uint32_t OD = 1u << 7; // output disable
    constexpr uint32_t IE = 1u << 6; // input enable
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_PADS_H
