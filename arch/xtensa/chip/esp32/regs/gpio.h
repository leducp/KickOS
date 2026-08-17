// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 GPIO + IO_MUX register map (ESP32 TRM GPIO / IO_MUX chapters), as
// used by the kernel diagnostic LED on GPIO2 (DOIT DevKit v1 / NodeMCU-32S blue
// LED, active-high).

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_GPIO_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_GPIO_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::gpio
{
    // GPIO bank-0 write-1-to-set / write-1-to-clear registers.
    constexpr uintptr_t OUT_W1TS = mmap::GPIO_BASE + 0x08u;
    constexpr uintptr_t OUT_W1TC = mmap::GPIO_BASE + 0x0Cu;
    constexpr uintptr_t ENABLE_W1TS = mmap::GPIO_BASE + 0x24u;

    // Per-pad IO_MUX register: IO_MUX_BASE + 0x40 selects GPIO2's pad function.
    constexpr uintptr_t IO_MUX_GPIO2 = mmap::IO_MUX_BASE + 0x40u;

    constexpr uint32_t LED_BIT = 1u << 2; // GPIO2
    // MCU_SEL=GPIO ([14:12]=2), DRV=2 ([11:10]=2), input disabled.
    constexpr uint32_t IO_MUX_GPIO_FUNC = (2u << 12) | (2u << 10);

    // IO_MUX word fields (for callers constructing `func`): MCU_SEL picks the pad
    // function (2 = GPIO matrix), FUN_IE enables the input buffer.
    constexpr uint32_t MCU_SEL_MASK = 0x7u << 12;
    constexpr uint32_t FUN_IE = 1u << 9;

    // GPIO-number -> IO_MUX register byte offset (from IO_MUX_BASE). The mapping is
    // SCRAMBLED in silicon (GPIO0 -> 0x44, GPIO2 -> 0x40, ...): it is NOT pin*4, so
    // it must be a literal table. 0 = no IO_MUX register on this GPIO number, i.e.
    // a nonexistent GPIO (20/24/28..31) or one unbonded on the WROOM module (37/38);
    // arch_pinmux_set returns EINVAL on a 0 entry. GPIO34..39 are INPUT-ONLY pads.
    constexpr uint16_t IO_MUX_OFF[40] = {
        0x44, 0x88, 0x40, 0x84, 0x48, 0x6C, 0x60, 0x64, // GPIO0..7
        0x68, 0x5C, 0x58, 0x54, 0x34, 0x38, 0x30, 0x3C, // GPIO8..15
        0x4C, 0x50, 0x70, 0x74, 0x00, 0x7C, 0x80, 0x8C, // GPIO16..23 (20 = none)
        0x00, 0x24, 0x28, 0x2C, 0x00, 0x00, 0x00, 0x00, // GPIO24..31 (24, 28..31 = none)
        0x1C, 0x20, 0x14, 0x18, 0x04, 0x00, 0x00, 0x10, // GPIO32..39 (37/38 unbonded on WROOM)
    };
}

#endif
