// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose start fails before its first spawn: its descriptor names a register
// block the task, which holds no window, was not given, so the bring-up refuses the instance.

#include <kickos/driver/declared/testfails.h>
#include <kickos/sys/driver_service.h>

namespace drv = kickos::driver;

namespace
{
    void service(void*)
    {
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the testfails descriptor is not a driver shape");
}

extern "C" int testfails_start(struct kos_driver_instance* instance)
{
    // The init's thread parks on the core its watcher shares, the declared one or the only one,
    // and resumes only once nothing above the init's priority is ready there: the watcher has
    // read the attempt before this one and waits again. Any length of park orders it.
    bool pinned = false;
    if (instance->core_mask != 0u)
    {
        int const rc = kos_thread_set_affinity(kos_thread_self(), instance->core_mask);
        if (rc != 0)
        {
            return rc;
        }
        pinned = true;
    }
    kos_sleep_ns(1000000ull);
    if (pinned)
    {
        (void)kos_thread_set_affinity(kos_thread_self(), 0u);
    }
    return drv::bring_up(k_desc, instance);
}
