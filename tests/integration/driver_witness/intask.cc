// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The client of testlate running as threads of its own task: each of five calls is answered before
// its deadline, the driver serving before this program runs. testlate's thread then exits, which
// fails this task during the sixth call. Its end ends the system.

#include "clients.h"

namespace
{
    // Far past testlate's exit after its fifth answer.
    constexpr int64_t INTASK_LAST_CALL_MS = 5000;
}

int main(int, char**)
{
    kos_cap_t const ep = kos_grant_endpoint("/svc/late");
    for (uint32_t k = 1; k <= 5u; k++)
    {
        uint32_t served = 0;
        int32_t const n = kos_call(ep, &served, 0u, sizeof(served),
                                   kos_after(kos_monotonic_now(), kos_ms(driver_witness::CALL_WAIT_MS)));
        if (n != static_cast<int32_t>(sizeof(served)))
        {
            printf("intask: call %lu answered %ld, not served\n", static_cast<unsigned long>(k),
                   static_cast<long>(n));
            return 0;
        }
        printf("intask: served %lu by /svc/late\n", static_cast<unsigned long>(served));
    }
    uint32_t served = 0;
    int32_t const n = kos_call(ep, &served, 0u, sizeof(served),
                               kos_after(kos_monotonic_now(), kos_ms(INTASK_LAST_CALL_MS)));
    printf("intask: call 6 answered %ld, the task outlived its driver\n", static_cast<long>(n));
}
