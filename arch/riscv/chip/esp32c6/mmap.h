// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6-WROOM-1 (ESP-RISC-V "HP CPU", RV32IMAC) peripheral base addresses.
// One home for the memory-mapped block bases the chip backend touches; the per-
// peripheral register maps live under regs/ and add their offsets onto these.
//
// Register addresses: ESP32-C6 TRM v1.2 (memory map Table 5.3-2; CLINT ch.1.7;
// PLIC/INTMTX section 1.6 + ch.10; watchdogs ch.14/15; UART ch.26; RMT ch.30;
// USB Serial/JTAG ch.32; access permission HP_APM/HP_TEE ch.16). Hand-rolled,
// no ESP-IDF/HAL sources.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_MMAP_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_MMAP_H

#include <stdint.h>

namespace kickos::esp32c6::mmap
{
    // --- CPU sub-system window (0x2000_0000). PLIC + core-local CLINT share the one
    //     4 KB page at 0x2000_1000 (TRM ch.1.7): PLIC at +0x000, CLINT at +0x800.
    constexpr uintptr_t PLIC_MX_BASE = 0x20001000u; // M-mode PLIC (real IRQ controller)
    constexpr uintptr_t CLINT_BASE = 0x20001800u;   // MSIP switch doorbell + MTIME/MTIMECMP

    // --- HP peripherals (memory map Table 5.3-2).
    constexpr uintptr_t UART0_BASE = 0x60000000u;           // console UART (CH343P bridge)
    constexpr uintptr_t RMT_BASE = 0x60006000u;             // remote-control TX (diag LED)
    constexpr uintptr_t TIMG0_BASE = 0x60008000u;           // timer group 0 (MWDT)
    constexpr uintptr_t TIMG1_BASE = 0x60009000u;           // timer group 1 (MWDT)
    constexpr uintptr_t USB_SERIAL_JTAG_BASE = 0x6000F000u; // native USB console (unused)
    constexpr uintptr_t INTMTX_BASE = 0x60010000u;          // interrupt matrix (source routing)
    constexpr uintptr_t IO_MUX_BASE = 0x60090000u;          // pad function mux
    constexpr uintptr_t GPIO_BASE = 0x60091000u;            // GPIO matrix block
    constexpr uintptr_t PCR_BASE = 0x60096000u;             // power/clock/reset gate controller
    constexpr uintptr_t HP_TEE_BASE = 0x60098000u;          // HP TEE mode controller
    constexpr uintptr_t HP_APM_BASE = 0x60099000u;          // HP access-permission controller
    constexpr uintptr_t INTPRI_BASE = 0x600C5000u;          // local int ctrl (FROM_CPU source only)

    // --- LP (RTC) domain.
    constexpr uintptr_t RTC_WDT_BASE = 0x600B1C00u; // RWDT + super-watchdog (SWD)
}

#endif
