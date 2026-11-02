// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The driver bring-up of a system whose composition names no packaged driver.

#include <kickos/sys.h>
#include <kickos/sys/image.h>
#include <kickos/sys/task_enter.h>

// Every task image takes it, a program's included.
KOS_IMAGE_SYSTEM_OBJECT()

extern "C" void kickos_driver_task(kos_table_task const*, uintptr_t)
{
    kos_panic("a driver task in a system linked without the driver path");
}

extern "C" void kickos_driver_hosted(kos_task_args const*)
{
}
