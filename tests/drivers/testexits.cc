// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose one thread, its entry, serves five requests per instance and then
// exits, which ends its task.

#include <kickos/driver/declared/testexits.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;

namespace
{
    void service(void*)
    {
        int const rc = testdrivers::serve(KOS_SPAWN_DELEGATED_CAP0);
        if (rc < 0)
        {
            drv::trap();
        }
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the testexits descriptor is not a driver shape");
}

extern "C" int testexits_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
