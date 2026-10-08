// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose entry thread idles while a second thread serves five requests per
// instance and then fails: it traps, and the fault ends the whole task.

#include <kickos/driver/declared/testtraps.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;

namespace
{
    void entry(void*)
    {
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }

    void worker(void*)
    {
        (void)testdrivers::serve(KOS_SPAWN_DELEGATED_CAP0);
        drv::trap();
        // Not the entry thread: an exit ends it alone and leaves the task alive.
        kos_exit(1);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the testtraps descriptor is not a driver shape");
}

extern "C" int testtraps_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
