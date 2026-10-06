// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 IO_BANK0 register map (RP2040 datasheet, RP-008371-DS, 2.19): user-bank
// pin function select. Per-GPIO pair {STATUS, CTRL} at base + gpio*8; CTRL is the
// second word of the pair. CTRL.FUNCSEL[4:0] picks the pin function.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_IO_BANK0_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_IO_BANK0_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2040::reg::io_bank0
{
    // GPIOn_STATUS = base + n*STRIDE; GPIOn_CTRL = base + n*STRIDE + CTRL_OFFSET.
    constexpr uintptr_t STRIDE = 0x8u;
    constexpr uintptr_t CTRL_OFFSET = 0x4u;

    constexpr uintptr_t gpio_ctrl(uint32_t n) { return mmap::IO_BANK0_BASE + n * STRIDE + CTRL_OFFSET; }

    constexpr uint32_t FUNCSEL_SIO = 5u; // F5 = SIO (software GPIO)
}

#endif
