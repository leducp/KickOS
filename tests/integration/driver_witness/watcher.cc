// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The watcher of the three drivers and of the client of the one that never serves: each time the
// init tells it of a death, or of a task marked dependency-down, it prints what the status block
// holds of that task.

#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>

namespace
{
    // One bit of the init's notification per watched task.
    constexpr uint32_t WATCH_BITS = 32u;
}

extern "C" void watcher_main(kos_self_t const* self)
{
    kos_cap_t const events = kos_grant_notify(self, "/init/events");
    if (kos_notify_bind(events) != 0)
    {
        printf("watch: no events\n");
        return;
    }
    while (true)
    {
        uint32_t bits = 0;
        if (kos_notify_wait(events, ~0u, KOS_TIMEOUT_NONE, &bits) != 0)
        {
            continue;
        }
        for (uint32_t i = 0; i < WATCH_BITS; i++)
        {
            struct kos_task_status st;
            if ((bits & (1u << i)) == 0u or kos_task_status(self, i, &st) != 0)
            {
                continue;
            }
            char const* state = "gone";
            if (st.alive)
            {
                state = "alive";
            }
            if (st.dependency_down)
            {
                state = "down";
            }
            printf("watch: %s deaths %u left %u %s\n", st.name, static_cast<unsigned>(st.deaths),
                   static_cast<unsigned>(st.restarts_left), state);
        }
    }
}
