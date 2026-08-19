// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 IO_MUX registers (TRM v1.2 ch.7). Block 0x6009_0000. IO_MUX_GPIOn_REG =
// 0x0004 + 0x4*n (Reg 7.20): selects the pad function (MCU_SEL) and drive strength.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_IO_MUX_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_IO_MUX_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::io_mux
{
    constexpr uintptr_t GPIO8 = mmap::IO_MUX_BASE + 0x24u; // == IO_MUX_GPIOn_REG 0x04 + 0x4*8

    // Per-pin IO_MUX_GPIOn_REG address.
    inline constexpr uintptr_t gpio(uint32_t pin)
    {
        return mmap::IO_MUX_BASE + 0x04u + 0x4u * pin;
    }

    constexpr uint32_t MCU_SEL_GPIO = 1u << 12; // MCU_SEL=1 -> GPIO matrix function
    constexpr uint32_t FUN_DRV_2 = 2u << 10;    // ~20 mA drive
}

#endif
