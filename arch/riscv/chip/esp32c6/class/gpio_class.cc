// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "gpio_class.h"

#include "../regs/gpio.h"

namespace kickos
{
namespace esp32c6
{
namespace driver
{
    uint32_t gpio_out_read(uintptr_t gpio_base)
    {
        volatile uint32_t const& out =
            *reinterpret_cast<volatile uint32_t*>(gpio_base + reg::gpio::OUT_OFFSET);
        return out;
    }
}
}
}
