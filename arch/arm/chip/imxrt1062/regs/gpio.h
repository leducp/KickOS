// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 GPIO register map (RM ch.12), instanced for GPIO2.
//
// GPIO2 and GPIO7 drive the SAME pads and IOMUXC_GPR_GPR27 selects which owns each bit
// (RM 11.3.28); a write to the instance that does not own the bit is silently ignored.
// This file maps GPIO2, GPR27's reset owner.

#ifndef KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_GPIO_H
#define KICKOS_ARCH_ARM_CHIP_IMXRT1062_REGS_GPIO_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::imxrt1062::reg::gpio
{
    // RM 12.6.1. DR_SET and DR_CLEAR are write-only one-shot registers, so a fault handler
    // drives the LED with one absolute store and no read-modify-write.
    constexpr uintptr_t GPIO2_DR = mmap::GPIO2_BASE + 0x00u;
    constexpr uintptr_t GPIO2_GDIR = mmap::GPIO2_BASE + 0x04u;
    constexpr uintptr_t GPIO2_DR_SET = mmap::GPIO2_BASE + 0x84u;
    constexpr uintptr_t GPIO2_DR_CLEAR = mmap::GPIO2_BASE + 0x88u;
    constexpr uintptr_t GPIO2_DR_TOGGLE = mmap::GPIO2_BASE + 0x8Cu;

    // The diagnostic LED: GPIO2.IO03, pad GPIO_B0_03 at ALT5. The pad-to-function half
    // (GPIO_B0_03 ALT5 = GPIO2_IO03) is the RM's (ch.11 mux table); that this pad is where the
    // Teensy 4.1 puts its LED is a BOARD claim, backed by no schematic in the reference set.
    constexpr uint32_t DIAG_LED_PIN = 3u;
    constexpr uint32_t DIAG_LED_BIT = 1u << DIAG_LED_PIN;
}

#endif
