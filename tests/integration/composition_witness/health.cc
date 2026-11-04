// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The restart witness's health checker: examples/composition/health.cc's calls and decisions,
// each last reading printed with the region it came from, so above one core a foreign line
// ending in a digit cannot complete it across a split.

#include <kickos/sys.h>

#include "sample.h"

#include <stdint.h>
#include <stdio.h>

extern "C" void witness_own_claim(char const* task, uint32_t id);
extern "C" void witness_own_check(char const* task, uint32_t id);

int main(int, char**)
{
    witness_own_claim("health", 2u);
    kos_cap_t const events = kos_grant_notify("/init/events");
    kos_window_t const history_window = kos_grant_mem("/shm/history");
    auto const hist = static_cast<history const*>(kos_window_addr(history_window));
    if (kos_notify_bind(events) != 0 or hist == nullptr
        or kos_window_size(history_window) < sizeof(history))
    {
        return 0;
    }

    while (true)
    {
        witness_own_check("health", 2u);
        uint32_t bits = 0;
        if (kos_notify_wait(events, ~0u, KOS_TIMEOUT_NONE, &bits) != 0)
        {
            continue;
        }
        uint32_t last = 0;
        bool const have_last = history_latest(hist, &last);
        for (uint32_t i = 0; i < 32; i++)
        {
            struct kos_task_status st;
            if ((bits & (1u << i)) == 0 or kos_task_status(i, &st) != 0)
            {
                continue;
            }
            if (have_last)
            {
                printf("health: %s died, last reading %lu in /shm/history\n", st.name,
                       static_cast<unsigned long>(last));
            }
            if (st.alive)
            {
                printf("health: %s restarted (%u deaths, %u restarts left)\n", st.name,
                       static_cast<unsigned>(st.deaths), static_cast<unsigned>(st.restarts_left));
            }
            else
            {
                printf("health: %s is down for good, running degraded\n", st.name);
            }
        }
    }
}
