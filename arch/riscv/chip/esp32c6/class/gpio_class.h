// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO class-driver leaf. FREESTANDING and STATELESS: free functions taking
// the GPIO Matrix block base explicitly, no ctor/dtor, no mutable static state. Built
// as its own kickos_class_esp32c6 static lib on a bare include path (repo include/ plus
// this chip's register dir only, never kernel/include), so the SAME object links
// unchanged into BOTH the kernel and an unprivileged userspace GPIO driver.
//
// GPIO_OUT_REG (TRM v1.2 Reg 7.1) is defined once in the chip's register map
// (regs/gpio.h, reg::gpio::OUT_OFFSET); the .cc consumes that base-relative offset
// rather than carrying its own copy.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLASS_GPIO_CLASS_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_CLASS_GPIO_CLASS_H

#include <stdint.h>

namespace kickos
{
namespace esp32c6
{
namespace driver
{
    // Read back the GPIO output latch. gpio_base is the GPIO Matrix block base
    // (0x6009_1000), so a userspace driver can pass its granted window.
    uint32_t gpio_out_read(uintptr_t gpio_base);
}
}
}

#endif
