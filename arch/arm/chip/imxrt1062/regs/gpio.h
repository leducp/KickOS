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
}

#endif
