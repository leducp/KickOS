// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M10 GOLDEN EXAMPLE: does not build yet. `kos_self_t`, `kos_grant_endpoint` and
// `KOS_ECONNREFUSED` are what M10 adds; everything else is today's API.
//
// The app. It polls /svc/sensor and decides for itself whether the measurement is alive.
// Whether the sensor, its driver, the bus or the IPC path failed does not matter here: a
// frozen or missing measurement is dead either way, and only the consumer can judge that end
// to end. The policy below is this app's, not KickOS's.

#include <kickos/sys.h>

#include "sample.h"

#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr uint64_t PERIOD_NS = 100000000ull; // 10 readings a second
    constexpr uint32_t CALL_TIMEOUT_US = 50000u;
    // Identical readings in a row: 2 s, longer than the slowest sensor here, a clock that
    // moves once a second.
    constexpr unsigned FROZEN_AFTER = 20u;
}

extern "C" void app_main(kos_self_t const* self)
{
    kos_cap_t const sensor = kos_grant_endpoint(self, "/svc/sensor");

    uint32_t last = 0;
    unsigned same = 0;
    while (true)
    {
        struct sample s = {};
        int32_t const n = kos_call_timed(sensor, &s, 0u, sizeof(s), CALL_TIMEOUT_US);
        // No answer is ever a park: each code says what became of the request. A motor
        // controller would brake on all three.
        if (n == -KOS_EPIPE)
        {
            printf("sensor: died holding the request\n");
        }
        else if (n == -KOS_EAGAIN)
        {
            printf("sensor: restarting\n");
        }
        else if (n == -KOS_ECONNREFUSED)
        {
            printf("sensor: gone for good\n");
        }
        else if (n != static_cast<int32_t>(sizeof(s)))
        {
            printf("sensor: no reading (%d)\n", static_cast<int>(n));
        }
        else if (s.value == last)
        {
            same++;
            if (same == FROZEN_AFTER)
            {
                printf("sensor: measurement frozen at %lu\n", static_cast<unsigned long>(s.value));
            }
        }
        else
        {
            same = 0;
            last = s.value;
            printf("sensor: %lu\n", static_cast<unsigned long>(s.value));
        }
        kos_sleep_ns(PERIOD_NS);
    }
}
