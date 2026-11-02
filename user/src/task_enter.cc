// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/task_enter.h>

#include <kickos/chip_limits.h> // KICKOS_CHIP_ONE_TASK
#include <kickos/sys.h>
#include <kickos/sys/init.h>
#include <kickos/sys/table.h>

extern "C" kos_table_task const* kickos_task_enter(kos_task_args const* a)
{
#if KICKOS_INIT_IMAGE and not KICKOS_HAVE_ASPACE
    kickos_image_crt0();
#endif
    kickos_image_start_once();
    kickos_self_enter(a->decl);
    auto const tasks = reinterpret_cast<kos_table_task const*>(kickos_table + 1);
#if !KICKOS_CHIP_ONE_TASK
    if (tasks[a->decl].driver != KOS_TABLE_NONE)
    {
        kickos_driver_task(&tasks[a->decl], a->block);
    }
#endif
    kickos_driver_hosted(a);
    return &tasks[a->decl];
}

extern "C" void kickos_task_entry(void* args)
{
    auto const a = static_cast<kos_task_args*>(args);
    kos_table_task const* const t = kickos_task_enter(a);
#if KICKOS_INIT_IMAGE
    (void)t;
    kos_exit(1);
#else
    kickos_program_run(t, a);
#endif
}
