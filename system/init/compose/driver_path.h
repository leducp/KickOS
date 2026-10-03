// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver's share of the walk (docs/design-m10-target.md, section 2): its ring block,
// its start through its descriptor, and the console it may hold.

#ifndef KICKOS_SYSTEM_INIT_COMPOSE_DRIVER_PATH_H
#define KICKOS_SYSTEM_INIT_COMPOSE_DRIVER_PATH_H

#include "walk.h"

#include <kickos/sys.h>

#include <stdint.h>

namespace kickos::init::driver_path
{
    // At boot, in the order of the reservations: reserves and self-grants packaged driver task
    // `task`'s ring block (1.1), which its record keeps for every start.
    void reserve(Walk& walk, uint16_t task);

    // Starts packaged driver task `task` through its catalogue START, with a cfg filled from the
    // table and the instance its record and table entry make, its watch armed with `badged`,
    // which the walk closes after. Writes the task the bring-up created to *out, KOS_TASK_NONE for
    // none, and answers 0, or nonzero for a failed start (1.6).
    int start(Walk& walk, uint16_t task, kos_cap_t badged, kos_task_t* out);

    // Whether `task` takes the console, whose endpoint the end of its handover narrows (2.1)
    // rather than the boot.
    bool narrows_at_handover(Walk const& walk, uint16_t task);

    // Before the system ends, where a console driver holds stdout: two zero-length sends, each
    // bounded by KOS_DRV_HANDOVER_PROBE_US (1.5).
    void drain(Walk const& walk);
}

#endif
