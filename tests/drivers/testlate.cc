// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose one thread, its entry, settles for longer than a client's call waits
// before its first receive, then serves five requests per instance and exits.

#include <kickos/driver/declared/testlate.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;

namespace
{
    // Past the driver witness's call timeout.
    constexpr uint64_t SETTLE_NS = 300000000ull;

    void service(void*)
    {
        kos_sleep_ns(SETTLE_NS);
        int const rc = testdrivers::serve(KOS_SPAWN_DELEGATED_CAP0);
        if (rc < 0)
        {
            drv::trap();
        }
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the testlate descriptor is not a driver shape");
}

extern "C" int testlate_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
