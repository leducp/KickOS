// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 GPIO class-driver leaf. FREESTANDING and STATELESS: no ctor/dtor, no mutable
// static state, the block base passed in. Built as kickos_class_esp32c6 on a bare include path
// (repo include/ plus this chip's register dir only), so the SAME object links into both the
// kernel and an unprivileged userspace GPIO driver.

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
