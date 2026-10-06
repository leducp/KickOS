// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "driver_path.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/service.h>

namespace kickos::init::driver_path
{
    namespace
    {
        uint32_t block_flags(kos_table_task const& t)
        {
            if ((t.flags & KOS_TABLE_TASK_BLOCK_UNCACHED) != 0u)
            {
                return KOS_MEM_NOCACHE;
            }
            return 0u;
        }
    }

    void reserve(Walk& walk, uint16_t task)
    {
        kos_table_task const& t = walk.task(task);
        if (t.block == 0u)
        {
            return;
        }
        TaskRecord& r = walk.record(task);
        r.block = walk.reserve(t.block, "kos_ram_alloc of the ring block", task);
        walk.check(kos_mem_self_grant(r.block, t.block, block_flags(t)), "kos_mem_self_grant of the ring block",
                   task);
    }

    int start(Walk& walk, uint16_t task, kos_cap_t badged, kos_task_t* out)
    {
        kos_table_task const& t = walk.task(task);
        TaskRecord& r = walk.record(task);

        kos_driver_instance instance = {};
        instance.block = r.block;
        instance.block_size = t.block;
        instance.block_flags = block_flags(t);
        instance.core_mask = t.core_mask;
        instance.endpoint = r.endpoint;
        instance.watch = badged;
        instance.task = KOS_TASK_NONE;
        instance.console = narrows_at_handover(walk, task);

        kos_service_cfg cfg = {};
        cfg.name = walk.name(task);
        cfg.prio = t.priority;
        cfg.instance = &instance;
        bool window = false;
        uint16_t lines = 0;
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = walk.grant(task, k);
            if (g.kind == KOS_GRANT_WINDOW and not window)
            {
                cfg.mmio_base = static_cast<uintptr_t>(g.base);
                cfg.mmio_window = g.size;
                window = true;
            }
            if (g.kind == KOS_GRANT_LINE)
            {
                // In the order of the driver's line roles; a count past the descriptor's is
                // refused by the bring-up.
                if (lines < kickos::driver::KOS_DRV_LINES_MAX)
                {
                    instance.lines[lines].number = g.line;
                    instance.lines[lines].index = g.line_index;
                }
                lines++;
            }
        }
        instance.line_count = static_cast<uint8_t>(lines);
        if (lines > 0xFFu)
        {
            instance.line_count = 0xFFu;
        }

        int const rc = t.entry.driver(&cfg);
        *out = instance.task;
        return rc;
    }

    bool narrows_at_handover(Walk const& walk, uint16_t task)
    {
        return (walk.task(task).flags & KOS_TABLE_TASK_CONSOLE) != 0u;
    }

    void drain(Walk const& walk)
    {
        bool console = false;
        for (uint16_t i = 0; i < walk.task_count(); i++)
        {
            if (narrows_at_handover(walk, i))
            {
                console = true;
            }
        }
        if (not console)
        {
            return;
        }
        // The second send completes once the driver has taken the first, which a console's one
        // receiver does only after it has written the bytes before it; a failed send ends it.
        if (kos_send_timed(KOS_CAP_STDOUT, "", 0, kickos::driver::KOS_DRV_HANDOVER_PROBE_US) >= 0)
        {
            (void)kos_send_timed(KOS_CAP_STDOUT, "", 0, kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
        }
    }
}
