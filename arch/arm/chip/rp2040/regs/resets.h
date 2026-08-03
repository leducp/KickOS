// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RP2040 RESETS register map (RP2040 datasheet, RP-008371-DS, 2.14). Peripherals
// are held in reset at power-up; clear a RESET bit (via the atomic CLR alias,
// regs/atomic.h) then poll the matching RESET_DONE bit. A block's RESET_DONE only
// asserts once it has a running clock.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_REGS_RESETS_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_REGS_RESETS_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::rp2040::reg::resets
{
    constexpr uintptr_t RESET = mmap::RESETS_BASE + 0x0u;
    constexpr uintptr_t RESET_DONE = mmap::RESETS_BASE + 0x8u;

    constexpr uint32_t IO_BANK0 = 1u << 5;
    constexpr uint32_t PADS_BANK0 = 1u << 8;
    constexpr uint32_t PLL_SYS = 1u << 12;
    constexpr uint32_t PLL_USB = 1u << 13;
    constexpr uint32_t TIMER = 1u << 21;
    constexpr uint32_t UART0 = 1u << 22;
    constexpr uint32_t USBCTRL = 1u << 24;
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_REGS_RESETS_H
