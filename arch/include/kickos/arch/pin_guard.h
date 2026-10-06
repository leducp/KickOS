// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The compile-time check that a chip's arch_pinmux_set guard refuses exactly the pins its
// board's KICKOS_BOARD_KERNEL_PINS names, over every port and pin the call accepts.

#ifndef KICKOS_ARCH_PIN_GUARD_H
#define KICKOS_ARCH_PIN_GUARD_H

#include <stdint.h>

namespace kickos
{
    template <typename Refused, typename Listed>
    constexpr bool refuses_exactly(Refused refused, Listed listed, uint32_t ports, uint32_t pins)
    {
        for (uint32_t port = 0; port < ports; port++)
        {
            for (uint32_t pin = 0; pin < pins; pin++)
            {
                if (refused(port, pin) != listed(port, pin))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

#endif
