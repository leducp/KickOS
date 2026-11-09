// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 timer group T0 (TRM ch.14): offsets into TIMG0_BASE / TIMG1_BASE.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_TIMG_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_TIMG_H

#include <stdint.h>

namespace kickos::esp32c6::reg::timg
{
    constexpr uintptr_t T0CONFIG = 0x00u; // Register 14.1
    constexpr uintptr_t T0LO = 0x04u;
    constexpr uintptr_t T0UPDATE = 0x0Cu; // Register 14.4

    constexpr uint32_t T0_EN = 1u << 31;
    constexpr uint32_t T0_INCREASE = 1u << 30;
    constexpr uint32_t T0_DIVIDER_S = 13u; // [28:13]; 1 and 2 both divide by 2
    constexpr uint32_t T0_DIVCNT_RST = 1u << 12;
    constexpr uint32_t T0_UPDATE = 1u << 31; // cleared by the hardware once the latch is done
}

#endif
