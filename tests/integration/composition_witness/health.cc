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

extern "C" void health_main(kos_self_t const* self)
{
    kos_cap_t const events = kos_grant_notify(self, "/init/events");
    kos_window_t const history_window = kos_grant_mem(self, "/shm/history");
    auto const hist = static_cast<history const*>(kos_window_addr(history_window));
    if (kos_notify_bind(events) != 0 or hist == nullptr
        or kos_window_size(history_window) < sizeof(history))
    {
        return;
    }

    while (true)
    {
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
            if ((bits & (1u << i)) == 0 or kos_task_status(self, i, &st) != 0)
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
