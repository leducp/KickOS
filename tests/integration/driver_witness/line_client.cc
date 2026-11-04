// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Calls testline until it answers that its line 0 was raised. Its end ends the system.

#include "clients.h"

namespace
{
    // testline's own wait for its line; the client outlasts it by a margin.
    constexpr uint64_t LINE_WAIT_NS = 20000000000ull;
    constexpr uint32_t LINE_CALLS = static_cast<uint32_t>(LINE_WAIT_NS / driver_witness::RETRY_NS) + 500u;
}

int main(int, char**)
{
    using namespace driver_witness;
    kos_cap_t const ep = kos_grant_endpoint("/svc/line");
    uint32_t raised = 0;
    int32_t const answered = static_cast<int32_t>(sizeof(raised));
    for (uint32_t calls = 0; calls < LINE_CALLS; calls++)
    {
        if (kos_call(ep, &raised, 0u, sizeof(raised), CALL_TIMEOUT_US) == answered
            and raised != 0u)
        {
            printf("line: testline reports line 0 raised\n");
            return 0;
        }
        kos_sleep_ns(RETRY_NS);
    }
    printf("line: FAIL: testline never reported line 0 raised\n");
}
