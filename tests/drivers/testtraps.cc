// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose entry thread idles while a second thread serves five requests per
// instance and then fails: it traps, and the fault ends the whole task.

#include <kickos/driver/declared/testtraps.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testtraps;

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

    constexpr drv::Descriptor k_desc = {
        .tag = "[testtraps] ",
        .expected_base = 0,
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        .threads = {{.entry = entry,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 0,
                     .caps = {}},
                    {.entry = worker,
                     .name = declared::k_declared.thread_name[1],
                     .prio_delta = declared::k_declared.prio_delta[1],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the testtraps descriptor is not a driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the testtraps descriptor departs from its kickos_add_driver declaration");
}

extern "C" int testtraps_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
