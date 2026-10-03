// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A client of each driver: it calls until the driver is gone for good, printing each answer, the
// count of requests the instance answering it has served, followed by the endpoint it called.

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr uint32_t CALL_TIMEOUT_US = 100000u;
    constexpr uint64_t RETRY_NS = 10000000ull; // 10 ms

    void call_until_refused(kos_self_t const* self, char const* path, char const* who)
    {
        kos_cap_t const ep = kos_grant_endpoint(self, path);
        while (true)
        {
            uint32_t served = 0;
            int32_t const n = kos_call_timed(ep, &served, 0u, sizeof(served), CALL_TIMEOUT_US);
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

extern "C" void exits_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/exits", "exits");
}

extern "C" void traps_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/traps", "traps");
}

// Never started: the init starts a task once the servers it uses serve, and testfails never does.
extern "C" void fails_client_main(kos_self_t const* self)
{
    call_until_refused(self, "/svc/fails", "fails");
}
