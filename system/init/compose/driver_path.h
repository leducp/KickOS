// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver's share of the walk (docs/design-m10-target.md, section 2): its ring block,
// its create, and the console it may hold.

#ifndef KICKOS_SYSTEM_INIT_COMPOSE_DRIVER_PATH_H
#define KICKOS_SYSTEM_INIT_COMPOSE_DRIVER_PATH_H

#include "walk.h"

#include <kickos/sys.h>

#include <stdint.h>

namespace kickos::init::driver_path
{
    // The memory type a packaged driver task's ring block takes.
    inline uint32_t block_flags(kos_table_task const& t)
    {
        if ((t.flags & KOS_TABLE_TASK_BLOCK_UNCACHED) != 0u)
        {
            return KOS_MEM_NOCACHE;
        }
        return 0u;
    }

    // At boot, in the order of the reservations: reserves and self-grants packaged driver task
    // `task`'s ring block (1.1), which its record keeps for every start.
    void reserve(Walk& walk, uint16_t task);

    // Creates packaged driver task `task` from its declaration, with its ring block, its endpoint
    // and the notification and doorbell this start makes at the places its table entry states,
    // its watch armed with `badged`, which the walk closes after; a console's handover is then
    // narrowed and probed. Writes the task to *out, KOS_TASK_NONE for none, and answers 0, or
    // nonzero for a failed start (1.6).
    int start(Walk& walk, uint16_t task, kos_cap_t badged, kos_task_t* out);

    // Into the create of task `task`: the endpoint of each driver it runs, at the place its table
    // states, and the ring block of the one among them that has one as the task's shared block,
    // which admission refuses on a build that translates.
    void host(Walk& walk, uint16_t task, kos_cap_grant* caps, kos_task_params* p);

    // Whether `task` takes the console, whose endpoint the end of its handover narrows (2.1)
    // rather than the boot.
    bool narrows_at_handover(Walk const& walk, uint16_t task);

    // Before the system ends, where a console driver holds stdout: kos_console_flush, bounded by
    // KOS_DRV_HANDOVER_PROBE_US (1.5).
    void drain(Walk const& walk);
}

#endif
