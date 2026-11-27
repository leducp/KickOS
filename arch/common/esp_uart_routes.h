// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The logical lines an ESP32 or ESP32-C6 device-interrupt pass posts: one per routed UART whose
// source is asserted, each UART carrying its own line. A source posts its line and the driver
// holding the UART clears it.

#ifndef KICKOS_ARCH_COMMON_ESP_UART_ROUTES_H
#define KICKOS_ARCH_COMMON_ESP_UART_ROUTES_H

#include <stdint.h>

namespace kickos::esp
{
    struct UartRoute
    {
        int line;            // below 32
        uint32_t status_bit; // the bit of the word the pass reads that says the source asserts
    };

    // `asserted` is the word the pass read; `unmasked` holds bit `line` for each line not masked.
    // Returns the lines to post, bit `line` each.
    constexpr uint32_t lines_to_post(UartRoute const* routes, uint32_t count, uint32_t asserted,
                                     uint32_t unmasked)
    {
        uint32_t lines = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t const line = 1u << static_cast<uint32_t>(routes[i].line);
            if ((asserted & routes[i].status_bit) != 0u and (unmasked & line) != 0u)
            {
                lines = lines | line;
            }
        }
        return lines;
    }
}

#endif
