// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO class-driver leaf (M4 Rule 6, decision R-A). A FREESTANDING,
// STATELESS register-logic core: free functions taking the GPIO Matrix block base
// explicitly, no ctor/dtor, no mutable static state. Built as its own
// kickos_class_esp32c6 static lib on a bare include path (repo include/ + this
// chip's register dir ONLY -- never kernel/include), so the SAME object links
// unchanged into BOTH the kernel (Phase A timebase, deferred) and an unprivileged
// userspace GPIO driver. One shared read-only copy, so no writable state.
//
// The C6 chip layer keeps its register map inline (no shared chip header), so this
// leaf owns the one register it reads -- GPIO_OUT_REG (TRM v1.2 Reg 7.1, at block
// base + 0x04); it does not duplicate a definition that lives anywhere else.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLASS_GPIO_CLASS_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLASS_GPIO_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace esp32c6
{
namespace classdrv
{
    // GPIO_OUT_REG (TRM Reg 7.1): the output data latch for GPIO0..31.
    constexpr uintptr_t GPIO_OUT_OFFSET = 0x04u;

    // Read back the GPIO output latch. gpio_base is the GPIO Matrix block base
    // (0x6009_1000). Pure read -- side-effect-free. Shared by the kernel and a
    // userspace GPIO driver that reads back what it drove.
    uint32_t gpio_out_read(uintptr_t gpio_base);
}
}
}

#endif
