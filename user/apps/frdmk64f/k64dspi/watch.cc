// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The main of the K64F/DSPI0 slot witness: it waits for the holder's end on the init's report and
// then probes the slot from a task of its own, which must fault (holder.cc).

#include <kickos/apps/peer_run.h>

#include "witness.h"

using namespace k64dspi_witness;

namespace
{
    constexpr uint32_t HOLDER_END_US = 30000000u;
}

int main(int, char**)
{
    kos_cap_t const events = kos_grant_notify("/init/events");
    if (kos_notify_bind(events) != 0)
    {
        give_up("no /init/events to wait the holder's end on", -1);
    }
    uint32_t bits = 0;
    int const waited = kos_notify_wait(events, 1u, HOLDER_END_US, &bits);
    if (waited == -KOS_ETIMEDOUT)
    {
        kos::print("[k64dspi] FAIL: the holder did not end within 30 s\n");
        exit(1);
    }
    if (waited != 0 or (bits & 1u) == 0u)
    {
        give_up("the wait for the holder's end", waited);
    }

    kos_program_t prog = KOS_CAP_NONE;
    int rc = kos_program_find("k64dspi_probe", &prog);
    if (rc != 0)
    {
        give_up("probe program", rc);
    }
    kos_task_t t = KOS_TASK_NONE;
    kos_task_params params = {};
    rc = kickos::apps::create_peer(prog, params, &t);
    if (rc != 0)
    {
        give_up("create of the probe", rc);
    }
    kos::print("[k64dspi] the holder ended; a task holding no window reads MCR\n");
    int status = 0;
    while (kickos::apps::wait_dead(t, &status, 1000u) == -KOS_ETIMEDOUT)
    {
    }
    if ((kos_task_state(t, &status) & KOS_TASK_ENDED) != 0 and status == KOS_EXIT_FAULT)
    {
        kos::print("[k64dspi] PASS: the probe after the holder's end faulted\n");
    }
    else
    {
        say("[k64dspi] FAIL: the probe after the holder's end was not killed (status %d)\n",
            status);
    }
    exit(0);
}
