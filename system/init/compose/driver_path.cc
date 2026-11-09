// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "driver_path.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/emit.h> // kconsole_offer
#include <kickos/sys/image.h>

#include <string.h>

KOS_IMAGE_INIT_OBJECT()

namespace kickos::init::driver_path
{
    namespace
    {
        // What a start passes beside the endpoint, closed once the task holds its own copies.
        struct Passed
        {
            kos_cap_t note = KOS_CAP_NONE;
            kos_cap_t bell = KOS_CAP_NONE;
        };

        void drop(Walk& walk, Passed const& passed, uint16_t task)
        {
            if (passed.bell != KOS_CAP_NONE)
            {
                walk.check(kos_handle_close(passed.bell), "kos_handle_close of the doorbell", task);
            }
            if (passed.note != KOS_CAP_NONE)
            {
                walk.check(kos_handle_close(passed.note), "kos_handle_close of the notification", task);
            }
        }

        void say(char const* s)
        {
            (void)kickos::kconsole_offer(s, strlen(s));
        }

        // The end of a console's handover: the WAIT the publish seated goes, then a zero-length
        // rendezvous through stdout, bounded by the readiness wait its receiver may sit in first.
        int handover(Walk& walk, uint16_t task, kos_task_t* handle)
        {
            TaskRecord& r = walk.record(task);
            (void)kos_cap_narrow(r.endpoint, kickos::driver::KOS_DRV_HANDOVER_KEPT);
            uint32_t const ready_us = kickos::driver::KOS_DRV_READY_WAIT_MAX
                                      * (kickos::driver::KOS_DRV_READY_WAIT_NS / 1000u);
            int const probe = kos_send(KOS_CAP_STDOUT, "", 0,
                                       kickos::driver::KOS_DRV_HANDOVER_PROBE_US + ready_us);
            if (probe >= 0)
            {
                return 0;
            }
            // Its end is what gives the kernel its console back, so the report reaches the wire.
            if (kos_task_slay(*handle, kickos::driver::KOS_DRV_HANDOVER_PROBE_US) == 0)
            {
                *handle = KOS_TASK_NONE;
            }
            say("[");
            say(walk.name(task));
            say("] ERROR: the console handover probe was not taken\n");
            return probe;
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
        *out = KOS_TASK_NONE;

        // At the places the table states, which are the entry thread's layout of them.
        kos_cap_grant caps[KICKOS_MAX_SPAWN_GRANTS] = {};
        uint16_t dest[KICKOS_MAX_SPAWN_GRANTS] = {};
        uint8_t n = 0;
        Passed passed;
        int rc = 0;
        for (uint16_t k = 0; k < t.grant_count and rc == 0; k++)
        {
            kos_table_grant const& g = walk.grant(task, k);
            if (g.cap_slot == KOS_TABLE_NONE or n == KICKOS_MAX_SPAWN_GRANTS)
            {
                continue;
            }
            if (g.kind == KOS_GRANT_ENDPOINT_SERVE)
            {
                caps[n].source_cap = r.endpoint;
                caps[n].rights_mask = KOS_CAP_WAIT | KOS_CAP_TRANSFER;
            }
            else if (g.kind == KOS_GRANT_NOTIFICATION)
            {
                if (passed.note == KOS_CAP_NONE)
                {
                    rc = kos_notify_create(&passed.note);
                }
                caps[n].source_cap = passed.note;
                caps[n].rights_mask = KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;
                if (rc == 0 and g.badge != 0u)
                {
                    rc = kos_notify_badge(passed.note, g.badge - 1u, &passed.bell);
                    caps[n].source_cap = passed.bell;
                    caps[n].rights_mask = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;
                }
            }
            else
            {
                continue;
            }
            dest[n] = g.cap_slot;
            n++;
        }
        kos_task_t handle = KOS_TASK_NONE;
        if (rc == 0)
        {
            kos_task_params p = {};
            p.instance = walk.name(task);
            p.block = r.block;
            p.size = t.block;
            p.flags = block_flags(t);
            p.caps = caps;
            p.cap_dest = dest;
            p.cap_count = n;
            rc = kos_task_create(walk.program(), &p, START_WAIT_US, &handle);
        }
        drop(walk, passed, task);
        if (rc == 0)
        {
            // The task runs already: a watch armed after its end or its readiness reports it.
            walk.check(kos_task_watch(handle, badged, KOS_CAP_NONE), "kos_task_watch", task);
            if (narrows_at_handover(walk, task))
            {
                rc = handover(walk, task, &handle);
            }
        }
        else if (narrows_at_handover(walk, task))
        {
            (void)kos_cap_narrow(r.endpoint, kickos::driver::KOS_DRV_HANDOVER_KEPT);
        }
        *out = handle;
        return rc;
    }

    void host(Walk& walk, uint16_t task, kos_cap_grant* caps, kos_task_params* p)
    {
        kos_table_task const& t = walk.task(task);
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = walk.grant(task, k);
            if (g.kind != KOS_GRANT_ENDPOINT_HOSTED)
            {
                continue;
            }
            // The driver's threads take theirs from it, and the host probes it before its entry.
            kos_cap_grant& c = caps[g.cap_slot - KOS_SPAWN_DELEGATED_CAP0];
            c.source_cap = walk.record(g.target).endpoint;
            c.rights_mask = KOS_CAP_WAIT | KOS_CAP_TRANSFER | KOS_CAP_SIGNAL;
            kos_table_task const& h = walk.task(g.target);
            if (h.block != 0u)
            {
                p->block = walk.record(g.target).block;
                p->size = h.block;
                p->flags = block_flags(h);
            }
        }
    }

    bool narrows_at_handover(Walk const& walk, uint16_t task)
    {
        uint16_t const flags = walk.task(task).flags;
        return (flags & KOS_TABLE_TASK_CONSOLE) != 0u and (flags & KOS_TABLE_TASK_HOSTED) == 0u;
    }

    void drain(Walk const& walk)
    {
        bool console = false;
        for (uint16_t i = 0; i < walk.task_count(); i++)
        {
            uint16_t const flags = walk.task(i).flags;
            if ((flags & KOS_TABLE_TASK_CONSOLE) != 0u and (flags & KOS_TABLE_TASK_ON_REQUEST) == 0u)
            {
                console = true;
            }
        }
        if (not console)
        {
            return;
        }
        (void)kos_console_flush(kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
    }
}
