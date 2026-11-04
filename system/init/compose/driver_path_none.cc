// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The driver path of a system whose composition names no packaged driver: no task takes it.

#include "driver_path.h"

#include <kickos/sys/image.h>

KOS_IMAGE_INIT_OBJECT()

namespace kickos::init::driver_path
{
    void reserve(Walk&, uint16_t)
    {
    }

    int start(Walk&, uint16_t, kos_cap_t, kos_task_t*)
    {
        kos_panic("init: a driver task in a system linked without the driver path");
    }

    void host(Walk&, uint16_t, kos_cap_grant*, kos_task_params*)
    {
    }

    bool narrows_at_handover(Walk const&, uint16_t)
    {
        return false;
    }

    void drain(Walk const&)
    {
    }
}
