// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/task_trampoline.h>

#include <kickos/sys.h>
#include <kickos/sys/table.h>

#include <stdio.h>

namespace kickos::init
{
    void task_trampoline(void* self)
    {
        auto const task = static_cast<kos_self_t const*>(self);
        task->entry.task(task);
        // Never exit(): its handlers and destructors are the image's, which every task shares.
        fflush(stdout);
        fflush(stderr);
        kos_exit(0);
    }
}
