// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_COMMON_PIN_GUARD_H
#define KICKOS_ARCH_COMMON_PIN_GUARD_H

#include <stdint.h>

#include "board_pins.h"

namespace kickos
{
    constexpr bool board_pin_kernel_owned(uint32_t port, uint32_t pin)
    {
#define KICKOS_KERNEL_PIN(p, bit) or (port == (p) and pin == (bit))
        return false KICKOS_BOARD_KERNEL_PINS(KICKOS_KERNEL_PIN);
#undef KICKOS_KERNEL_PIN
    }
}

#endif
