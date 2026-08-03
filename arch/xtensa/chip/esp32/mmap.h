// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 (WROOM-32) peripheral base addresses (ESP32 TRM v5.8 Table 3.3-6,
// "Peripheral Address Mapping", chapter 3 System and Memory). Bases only; register
// offsets + fields live in regs/<periph>.h. Hand-rolled, no ESP-IDF/HAL sources.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_MMAP_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_MMAP_H

#include <stdint.h>

namespace kickos::esp32::mmap
{
    constexpr uintptr_t DPORT_BASE = 0x3FF00000u;    // interrupt matrix + CPU clock select
    constexpr uintptr_t UART0_BASE = 0x3FF40000u;    // TRM UART chapter
    constexpr uintptr_t GPIO_BASE = 0x3FF44000u;
    constexpr uintptr_t RTC_CNTL_BASE = 0x3FF48000u; // RTC control / clock / MWDT
    constexpr uintptr_t IO_MUX_BASE = 0x3FF49000u;

    // Timer groups are 0x1000 apart (TRM ch.10 "Timer Group (TIMG)").
    constexpr uintptr_t TIMG0_BASE = 0x3FF5F000u;
    constexpr uintptr_t TIMG1_BASE = 0x3FF60000u;

    // Analog-config gate region on the APB peripheral bus; ANA_CONFIG at +0x44
    // gates the analog blocks (bit 17 = BBPLL). Peripheral name unconfirmed against
    // the TRM; the classic-ESP32 code reaches it by absolute address.
    constexpr uintptr_t SYSCON_BASE = 0x6000E000u;

    // ROM routine entry points (fixed code addresses, NOT peripheral MMIO).
    // _regi2c_impl_write(block, host_id, reg_add, data): windowed-ABI ROM routine
    // that drives the internal analog reg-I2C bus (the BBPLL analog register file is
    // NOT memory-mapped). Same entry the IDF links.
    constexpr uintptr_t ROM_REGI2C_WRITE = 0x400041A4u;
}

#endif // KICKOS_ARCH_XTENSA_CHIP_ESP32_MMAP_H
