// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The device-interrupt pass over one chip's own UART_ROUTES: a UART's asserted source posts its
// own line alone, whichever UART is the console.

#ifndef KICKOS_TESTS_UNIT_ESPUARTROUTE_ROUTES_H
#define KICKOS_TESTS_UNIT_ESPUARTROUTE_ROUTES_H

#include "esp_uart_routes.h"

#include <gtest/gtest.h>

#include <stdint.h>

namespace espuartroute
{
    constexpr uint32_t ALL = ~0u;

    template <uint32_t N>
    void each_posts_its_own(kickos::esp::UartRoute const (&routes)[N])
    {
        for (uint32_t i = 0; i < N; i++)
        {
            EXPECT_EQ(kickos::esp::lines_to_post(routes, N, routes[i].status_bit, ALL), 1u << routes[i].line)
                << "UART" << i;
        }
    }

    template <uint32_t N>
    void all_post_all(kickos::esp::UartRoute const (&routes)[N])
    {
        uint32_t asserted = 0;
        uint32_t lines = 0;
        for (uint32_t i = 0; i < N; i++)
        {
            asserted = asserted | routes[i].status_bit;
            lines = lines | (1u << routes[i].line);
        }
        EXPECT_EQ(kickos::esp::lines_to_post(routes, N, asserted, ALL), lines);
        for (uint32_t i = 0; i < N; i++)
        {
            uint32_t const masked = 1u << routes[i].line;
            EXPECT_EQ(kickos::esp::lines_to_post(routes, N, asserted, ALL & ~masked), lines & ~masked)
                << "UART" << i;
        }
    }

    template <uint32_t N>
    void none_posts_none(kickos::esp::UartRoute const (&routes)[N])
    {
        uint32_t asserted = 0;
        for (uint32_t i = 0; i < N; i++)
        {
            asserted = asserted | routes[i].status_bit;
        }
        EXPECT_EQ(kickos::esp::lines_to_post(routes, N, ~asserted, ALL), 0u);
    }
}

#endif
