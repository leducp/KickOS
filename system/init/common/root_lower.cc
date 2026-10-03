// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos_root_lower for an image linking no system target (<kickos/sys/init.h>).

#include <kickos/config/priorities.h>
#include <kickos/sys.h>
#include <kickos/sys/init.h>

extern "C" void kickos_root_lower(void)
{
    if (kos_thread_set_priority(KICKOS_PRIO_ROOT) != 0)
    {
        kos_panic("root: kos_thread_set_priority refused");
    }
}
