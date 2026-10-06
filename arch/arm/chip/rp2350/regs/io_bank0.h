// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2350 IO_BANK0 register map (RP2350 datasheet RP-008373-DS-2, 9.11.1): per-pin
// function select. Offsets are IO_BANK0_BASE-relative (see mmap.h).

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_REGS_IO_BANK0_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_REGS_IO_BANK0_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::rp2350::reg::io_bank0
{
    // GPIOn_CTRL = IO_BANK0_BASE + 0x04 + n*0x08.
    constexpr uintptr_t CTRL_BASE = mmap::IO_BANK0_BASE + 0x04u;
    constexpr uintptr_t CTRL_STRIDE = 0x08u;

    constexpr uintptr_t gpio_ctrl(uint32_t n) { return CTRL_BASE + n * CTRL_STRIDE; }

    constexpr uint32_t FUNCSEL_SIO = 5u; // F5 = SIO (software GPIO)
}

#endif
