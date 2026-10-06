// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The packaged drivers the xmc4800-relax golden runs, as descriptors of the shapes their
// catalogue entries declare, started through the real bring-up over the scripted kernel.

#ifndef KICKOS_TESTS_UNIT_INITWALK_DRIVERS_H
#define KICKOS_TESTS_UNIT_INITWALK_DRIVERS_H

#include "fake_kernel.h"

namespace drivers
{
    // What a spawned driver thread does that the init sees: the console's IRQ thread sets its
    // ring block's readiness latch unless `latch` is false. With `uncached_console` the console's
    // descriptor types its ring block KOS_MEM_NOCACHE.
    struct Behaviour
    {
        bool latch = true;
        bool uncached_console = false;
    };

    // Reset with each run of the walk.
    extern Behaviour behaviour;

    // The fake's spawn hook.
    void spawned(fake::Thread const& thread);
}

#endif
