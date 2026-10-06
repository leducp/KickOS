// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose one thread, its entry, serves five requests per instance and then
// exits, which ends its task.

#include <kickos/driver/declared/testexits.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testexits;

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

    constexpr drv::Descriptor k_desc = {
        .tag = "[testexits] ",
        .expected_base = 0,
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        .threads = {{.entry = service,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the testexits descriptor is not a driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the testexits descriptor departs from its kickos_add_driver declaration");
}

extern "C" int testexits_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
