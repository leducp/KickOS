// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 GPIO + IO_MUX register map (ESP32 TRM GPIO / IO_MUX chapters), as
// used by the kernel diagnostic LED on GPIO2 (DOIT DevKit v1 / NodeMCU-32S blue
// LED, active-high).

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_GPIO_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_GPIO_H

#include "../mmap.h"

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
}

#endif // KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_GPIO_H
