// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 (FRDM-K64F) peripheral base addresses (K64 Sub-Family Reference
// Manual, K64P144M120SF5RM). Bases only; register offsets + fields live in
// regs/<periph>.h. Hand-rolled, no vendor CMSIS pack.

#ifndef KICKOS_ARCH_ARM_CHIP_MK64F_MMAP_H
#define KICKOS_ARCH_ARM_CHIP_MK64F_MMAP_H

#include <stdint.h>

namespace kickos::mk64f::mmap
{
    constexpr uintptr_t SYSMPU_BASE = 0x4000D000u; // bus-side MPU (RM ch.19)
    constexpr uintptr_t DSPI0_BASE = 0x4002C000u;  // SPI (RM ch.50); bus-clocked
    constexpr uintptr_t PIT_BASE = 0x40037000u;    // periodic timer (RM ch.44)
    constexpr uintptr_t SIM_BASE = 0x40048000u;    // clock gates / dividers (RM ch.12)

    // PORTx pin-control: PORTA + port*0x1000 (A=0..E=4).
    constexpr uintptr_t PORTA_BASE = 0x40049000u;
    constexpr uintptr_t PORTB_BASE = 0x4004A000u;
    constexpr uintptr_t PORT_STRIDE = 0x1000u;

    constexpr uintptr_t WDOG_BASE = 0x40052000u; // watchdog (RM ch.24)
    constexpr uintptr_t MCG_BASE = 0x40064000u;  // clock generator / PLL (RM ch.25)
    constexpr uintptr_t OSC0_BASE = 0x40065000u; // oscillator (RM ch.26)

    // UART instances (RM ch.52). UART0/UART1 are system-clocked; UART2..4 bus-clocked.
    constexpr uintptr_t UART0_BASE = 0x4006A000u;
    constexpr uintptr_t UART1_BASE = 0x4006B000u;
    constexpr uintptr_t UART2_BASE = 0x4006C000u;
    constexpr uintptr_t UART3_BASE = 0x4006D000u;
    constexpr uintptr_t UART4_BASE = 0x400EA000u;

    // GPIOx controller: GPIOA + port*0x40 (A=0..E=4).
    constexpr uintptr_t GPIOA_BASE = 0x400FF000u;
    constexpr uintptr_t GPIOB_BASE = 0x400FF040u;
    constexpr uintptr_t GPIO_STRIDE = 0x40u;
}

#endif // KICKOS_ARCH_ARM_CHIP_MK64F_MMAP_H
