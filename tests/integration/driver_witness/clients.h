// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the driver witness's client programs share: a client calls its driver for good, printing
// each answer, the count of requests the instance answering it has served, followed by the
// endpoint it called, and one `refused` line each time the driver stops serving. A refusal reads
// the same while the driver restarts as once it is gone for good.

#ifndef KICKOS_TESTS_INTEGRATION_DRIVER_WITNESS_CLIENTS_H
#define KICKOS_TESTS_INTEGRATION_DRIVER_WITNESS_CLIENTS_H

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdint.h>
#include <stdio.h>

namespace driver_witness
{
    constexpr int64_t CALL_WAIT_MS = 100;
    constexpr int64_t RETRY_NS = 10000000; // 10 ms

    [[noreturn]] inline void call_while_served(char const* path, char const* who)
    {
        kos_cap_t const ep = kos_grant_endpoint(path);
        bool refused = false;
        while (true)
        {
            uint32_t served = 0;
            int32_t const n = kos_call(ep, &served, 0u, sizeof(served),
                                       kos_after(kos_monotonic_now(), kos_ms(CALL_WAIT_MS)));
            if (n == static_cast<int32_t>(sizeof(served)))
            {
                printf("%s: served %lu by %s\n", who, static_cast<unsigned long>(served), path);
                refused = false;
                continue;
            }
            if (n == -KOS_ECONNREFUSED and not refused)
            {
                printf("%s: refused\n", who);
                refused = true;
            }
            kos_sleep_for(kos_ns(RETRY_NS));
        }
    }
}

#endif
