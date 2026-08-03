// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2040 (Pico) peripheral base addresses (RP2040 datasheet,
// RP-008371-DS). Bases only; register offsets + fields live in regs/<periph>.h.
// Clean-room from the datasheet, no vendor SDK sources.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2040_MMAP_H
#define KICKOS_ARCH_ARM_CHIP_RP2040_MMAP_H

#include <stdint.h>

namespace kickos::rp2040::mmap
{
    // APB peripherals. Each is mirrored by the atomic SET/CLR/XOR alias window
    // (see regs/atomic.h): a full 0x4000 region per block covers base + aliases.
    constexpr uintptr_t CLOCKS_BASE = 0x40008000u;   // clock generators (DS 2.15)
    constexpr uintptr_t RESETS_BASE = 0x4000c000u;   // peripheral reset control (DS 2.14)
    constexpr uintptr_t IO_BANK0_BASE = 0x40014000u; // user-bank pin mux (DS 2.19)
    constexpr uintptr_t PADS_BANK0_BASE = 0x4001c000u; // user-bank pad control (DS 2.19.6)
    constexpr uintptr_t XOSC_BASE = 0x40024000u;     // 12 MHz crystal oscillator (DS 2.16)
    constexpr uintptr_t PLL_SYS_BASE = 0x40028000u;  // system PLL (DS 2.18)
    constexpr uintptr_t PLL_USB_BASE = 0x4002c000u;  // USB PLL, 48 MHz ceiling (DS 2.18)
    constexpr uintptr_t UART0_BASE = 0x40034000u;    // ARM PL011 UART0 (DS 4.2)
    constexpr uintptr_t TIMER_BASE = 0x40054000u;    // 64-bit us monotonic timer (DS 4.6)
    constexpr uintptr_t WATCHDOG_BASE = 0x40058000u; // watchdog + TICK generator (DS 4.7)

    // Single-cycle IO (SIO): on the core-local IOPORT bus, NOT the APB. It has its
    // own SET/CLR/XOR registers and does NOT use the +0x2000/+0x3000 alias window.
    constexpr uintptr_t SIO_BASE = 0xd0000000u; // single-cycle IO / GPIO (DS 2.3.1)

    // USB device controller (DS 2.2.2, 4.1). On the AHB-Lite, not the APB: the 4 KiB
    // DPRAM sits at the base and the register block 0x10000 above it. The DPRAM takes
    // 8/16/32-bit accesses and has NO set/clear aliases (DS 4.1.2.7).
    constexpr uintptr_t USBCTRL_DPRAM_BASE = 0x50100000u;
    constexpr uintptr_t USBCTRL_REGS_BASE = 0x50110000u;
    // 128 KiB covering both. PMSAv6 needs a power-of-two naturally-aligned region and
    // 0x50100000 is 128 KiB-aligned; the whole span is the USB block's own AHB slot.
    constexpr uintptr_t USBCTRL_WINDOW = 0x20000u;

    // Synopsys SSI (QSPI flash controller) fronting the XIP window. Configured by
    // boot2 for execute-in-place; runs on the AHB, no atomic alias window.
    constexpr uintptr_t XIP_SSI_BASE = 0x18000000u; // XIP SSI (DS 4.10.13)
}

#endif // KICKOS_ARCH_ARM_CHIP_RP2040_MMAP_H
