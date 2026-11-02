// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the driver witness's client programs share: a client calls until its driver is gone for
// good, printing each answer, the count of requests the instance answering it has served,
// followed by the endpoint it called.

#ifndef KICKOS_TESTS_INTEGRATION_DRIVER_WITNESS_CLIENTS_H
#define KICKOS_TESTS_INTEGRATION_DRIVER_WITNESS_CLIENTS_H

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdint.h>
#include <stdio.h>

namespace driver_witness
{
    constexpr uint32_t CALL_TIMEOUT_US = 100000u;
    constexpr uint64_t RETRY_NS = 10000000ull; // 10 ms

    inline void call_until_refused(char const* path, char const* who)
    {
        kos_cap_t const ep = kos_grant_endpoint(path);
        while (true)
        {
            uint32_t served = 0;
            int32_t const n = kos_call(ep, &served, 0u, sizeof(served), CALL_TIMEOUT_US);
            if (n == -KOS_ECONNREFUSED)
            {
                printf("%s: refused\n", who);
                return;
            }
            if (n == static_cast<int32_t>(sizeof(served)))
            {
                printf("%s: served %lu by %s\n", who, static_cast<unsigned long>(served), path);
                continue;
            }
            kos_sleep_ns(RETRY_NS);
        }
    }
}

#endif
